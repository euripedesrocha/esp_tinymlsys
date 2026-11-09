// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/operations.hpp"
#include "esp_execution/let_value.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <algorithm>
#include <array>
#include <cstdint>

namespace {
namespace ex = esp::execution;

// Draft capability probes, not peripheral or network driver implementations.
// A byte stream may complete arbitrary prefixes, independently of message sizes.
struct fragmented_stream {
    auto async_read_some(std::span<std::byte> buffer) {
        return ex::just() | ex::let_value([buffer]() noexcept {
                   auto count = std::min(std::size_t{2}, buffer.size());
                   std::fill_n(buffer.begin(), count, std::byte{7});
                   return ex::just(std::optional<std::error_code>{}, count);
               });
    }
};

// An I2S-style adapter reports bytes but only transfers whole aligned samples.
// Satisfying the syntactic capability does not make every byte span valid.
struct aligned_samples {
    struct source {
        using sender_concept = ex::sender_tag;
        std::span<std::byte> buffer;
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t)>;
        auto get_env() const noexcept { return ex::empty_env{}; }
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            std::span<std::byte> buffer;
            Receiver receiver;
            void start() & noexcept {
                if (reinterpret_cast<std::uintptr_t>(buffer.data()) % alignof(std::uint32_t) ||
                    buffer.size() < sizeof(std::uint32_t)) {
                    ex::set_value(std::move(receiver),
                                  std::optional{std::make_error_code(std::errc::invalid_argument)},
                                  std::size_t{0});
                    return;
                }
                std::fill_n(buffer.begin(), sizeof(std::uint32_t), std::byte{9});
                ex::set_value(std::move(receiver), std::optional<std::error_code>{},
                              sizeof(std::uint32_t));
            }
        };
        auto connect(ex::receiver auto receiver) const {
            return operation<decltype(receiver)>{buffer, std::move(receiver)};
        }
    };
    auto async_read_some(std::span<std::byte> buffer) { return source{buffer}; }
};

struct result {
    unsigned values = 0, errors = 0;
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    result* output;
    auto get_env() const noexcept { return ex::empty_env{}; }
    void set_value() && noexcept { ++output->values; }
    void set_error(std::error_code error) && noexcept {
        CHECK(error == std::errc::invalid_argument);
        ++output->errors;
    }
    void set_error(std::exception_ptr) && noexcept {
        FAIL_CHECK("Unexpected completion in I/O test");
    }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
};
} // namespace

SCENARIO("Exact I/O preserves transport constraints", "[io][transport]") {
    alignas(std::uint32_t) std::array<std::byte, 8> bytes;
    bytes.fill(std::byte{0xff});
    result output;

    GIVEN("a byte stream that acknowledges at most two bytes per read") {
        fragmented_stream stream;
        STATIC_REQUIRE(ex::io::readable<fragmented_stream>);

        WHEN("an exact read requests six bytes of an eight-byte destination") {
            auto sender = ex::io::async_read(stream, std::span{bytes}.first(6));
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the requested prefix is filled and the remaining bytes stay untouched") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(std::all_of(bytes.begin(), bytes.begin() + 6,
                                  [](auto byte) { return byte == std::byte{7}; }));
                CHECK((bytes[6] == std::byte{0xff} && bytes[7] == std::byte{0xff}));
            }
        }
    }

    GIVEN("an endpoint accepting only complete aligned four-byte samples") {
        aligned_samples samples;
        STATIC_REQUIRE(ex::io::readable<aligned_samples>);

        WHEN("an exact read requests two complete aligned samples") {
            auto sender = ex::io::async_read(samples, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("all requested samples are delivered") {
                CHECK(output.values == 1);
                CHECK(output.errors == 0);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{9}; }));
            }
        }

        WHEN("an exact read leaves an incomplete sample after its first acknowledgement") {
            auto sender = ex::io::async_read(samples, std::span{bytes}.first(6));
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the transport error preserves the acknowledged prefix") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(std::all_of(bytes.begin(), bytes.begin() + 4,
                                  [](auto byte) { return byte == std::byte{9}; }));
                CHECK(std::all_of(bytes.begin() + 4, bytes.end(),
                                  [](auto byte) { return byte == std::byte{0xff}; }));
            }
        }

        WHEN("an exact read supplies an unaligned destination") {
            auto sender = ex::io::async_read(samples, std::span{bytes}.subspan(1, 4));
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the transport error preserves the acknowledged prefix") {
                CHECK(output.values == 0);
                CHECK(output.errors == 1);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{0xff}; }));
            }
        }
    }
}
