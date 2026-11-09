// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/operations.hpp"
#include "esp_execution/let_value.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <array>
#include <algorithm>

namespace {
namespace ex = esp::execution;
struct io_key {};
struct environment {
    int query(io_key) const noexcept { return 42; }
};
template <class Sender> struct gated_sender {
    using sender_concept = ex::sender_tag;
    Sender source;
    template <class Env>
        requires requires(const Env& env) { env.query(io_key{}); }
    using completion_signatures = ex::completion_signatures_of_t<Sender, Env>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::detail::receiver_for_sender<gated_sender> auto receiver) const {
        return ex::connect(source, std::move(receiver));
    }
};
struct device {
    unsigned reads = 0, writes = 0;
    auto async_read_some(std::span<std::byte> buffer) {
        return gated_sender{ex::just() | ex::let_value([this, buffer]() noexcept {
                                ++reads;
                                buffer.front() = std::byte{7};
                                return ex::just(std::optional<std::error_code>{}, std::size_t{1});
                            })};
    }
    auto async_write_some(std::span<const std::byte>) {
        return gated_sender{ex::just() | ex::let_value([this]() noexcept {
                                ++writes;
                                return ex::just(std::optional<std::error_code>{}, std::size_t{1});
                            })};
    }
};
struct native_device {
    unsigned factories = 0, queries = 0;
    auto async_query(std::span<const std::byte>, std::span<std::byte> response) {
        ++factories;
        return gated_sender{ex::just() | ex::let_value([this, response]() noexcept {
                                ++queries;
                                for (auto& byte : response)
                                    byte = std::byte{9};
                                return ex::just();
                            })};
    }
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    unsigned* values;
    auto get_env() const noexcept { return environment{}; }
    void set_value() && noexcept { ++*values; }
    void set_error(auto) && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
};
} // namespace

SCENARIO("Exact I/O operations propagate the receiver environment", "[io][environment]") {
    GIVEN("an endpoint that acknowledges one byte at a time and requires the I/O environment") {
        device object;
        std::array<std::byte, 3> bytes{};
        unsigned values = 0;

        WHEN("an exact read is started directly") {
            auto sender = ex::io::async_read(object, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 0);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }

        WHEN("an exact write is started directly") {
            auto sender = ex::io::async_write(object, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 0);
                CHECK(object.writes == 3);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{0}; }));
            }
        }

        WHEN("an exact read follows a buffer sender") {
            auto sender = ex::just(std::span{bytes}) | ex::io::async_read(object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 0);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }

        WHEN("a composed query is started directly") {
            auto sender = ex::io::async_query(object, bytes, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 3);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }

        WHEN("a composed query follows a command and response sender") {
            auto sender =
                ex::just(std::span{bytes}, std::span{bytes}) | ex::io::async_query(object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 3);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }

        WHEN("an exact write follows an exact read") {
            auto sender = ex::io::async_read(object, bytes) | ex::let_value([&object, &bytes] {
                              return ex::io::async_write(object, bytes);
                          });
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 3);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }

        WHEN("an exact read follows a predecessor requiring the same environment") {
            auto sender = gated_sender{ex::just(std::span{bytes})} | ex::io::async_read(object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the receiver environment reaches every transfer and the operation completes "
                 "once") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.reads == 3);
                CHECK(object.writes == 0);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{7}; }));
            }
        }
    }
}

SCENARIO("Native queries propagate the receiver environment", "[io][environment]") {
    GIVEN("a native query endpoint whose sender requires the receiver's I/O environment") {
        native_device object;
        std::array<std::byte, 3> bytes{};
        unsigned values = 0;

        WHEN("a native query is connected and started directly") {
            auto sender = ex::io::async_query(object, bytes, bytes);
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("the receiver environment enables exactly one native query") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.factories == 1);
                CHECK(object.queries == 1);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{9}; }));
            }
        }

        WHEN("a native query follows an explicit command and response sender") {
            auto sender = ex::io::async_query(ex::just(std::span{bytes}, std::span{bytes}), object);
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("the receiver environment enables exactly one native query") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.factories == 1);
                CHECK(object.queries == 1);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{9}; }));
            }
        }

        WHEN("a native query follows a command and response sender using the pipe form") {
            auto sender =
                ex::just(std::span{bytes}, std::span{bytes}) | ex::io::async_query(object);
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("the receiver environment enables exactly one native query") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.factories == 1);
                CHECK(object.queries == 1);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{9}; }));
            }
        }

        WHEN("a native query follows a predecessor requiring the same environment") {
            auto sender = gated_sender{ex::just(std::span{bytes}, std::span{bytes})} |
                          ex::io::async_query(object);
            CHECK(object.factories == 0);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            CHECK(object.factories == 0);
            ex::start(operation);

            THEN("the receiver environment enables exactly one native query") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.factories == 1);
                CHECK(object.queries == 1);
                CHECK(std::all_of(bytes.begin(), bytes.end(),
                                  [](auto byte) { return byte == std::byte{9}; }));
            }
        }
    }
}
