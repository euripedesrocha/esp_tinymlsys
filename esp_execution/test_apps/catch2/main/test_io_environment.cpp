// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/primitives.hpp"
#include "esp_execution/let_value.hpp"
#include <catch2/catch_test_macros.hpp>
#include <optional>
#include <system_error>
#include <array>

namespace {
namespace ex = esp::execution;
struct io_key {};
struct environment {
    int query(io_key) const noexcept { return 42; }
};
template <class Env>
concept supports_io = requires(const Env& env) { env.query(io_key{}); };

struct source {
    using sender_concept = ex::sender_tag;
    unsigned* calls;
    template <supports_io Env>
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::detail::receiver_for_sender<source> auto receiver) const {
        return ex::connect(ex::just() | ex::let_value([calls = calls]() noexcept {
                               ++*calls;
                               return ex::just(std::optional<std::error_code>{}, std::size_t{1});
                           }),
                           std::move(receiver));
    }
};
struct device {
    unsigned calls = 0;
    auto async_read_some(std::span<std::byte>) { return source{&calls}; }
    auto async_write_some(std::span<const std::byte>) { return source{&calls}; }
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    unsigned* values;
    auto get_env() const noexcept { return environment{}; }
    void set_value(std::optional<std::error_code> error, std::size_t n) && noexcept {
        CHECK(!error);
        CHECK(n == 1);
        ++*values;
    }
    void set_error(auto) && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
    void set_stopped() && noexcept { FAIL_CHECK("Unexpected completion in I/O test"); }
};
struct buffer_source {
    using sender_concept = ex::sender_tag;
    std::span<std::byte> buffer;
    template <supports_io Env>
    using completion_signatures = ex::completion_signatures<ex::set_value_t(std::span<std::byte>)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::detail::receiver_for_sender<buffer_source> auto receiver) const {
        return ex::connect(ex::just(buffer), std::move(receiver));
    }
};
} // namespace

SCENARIO("I/O primitives obtain their required environment from the receiver",
         "[io][environment]") {
    GIVEN("an endpoint whose senders require the receiver's I/O environment") {
        device object;
        std::array<std::byte, 1> bytes{};
        unsigned values = 0;

        WHEN("a read is connected and started using the direct form") {
            auto sender = ex::io::async_read_some(object, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the supplied environment enables exactly one transfer") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.calls == 1);
            }
        }

        WHEN("a write is connected and started using the direct form") {
            auto sender = ex::io::async_write_some(object, bytes);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the supplied environment enables exactly one transfer") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.calls == 1);
            }
        }

        WHEN("a read follows a buffer sender using the explicit predecessor form") {
            auto sender = ex::io::async_read_some(ex::just(std::span{bytes}), object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the supplied environment enables exactly one transfer") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.calls == 1);
            }
        }

        WHEN("a write follows a buffer sender using the pipe form") {
            auto sender = ex::just(std::span{bytes}) | ex::io::async_write_some(object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the supplied environment enables exactly one transfer") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.calls == 1);
            }
        }

        WHEN("a read follows a predecessor that also requires the I/O environment") {
            auto sender = buffer_source{bytes} | ex::io::async_read_some(object);
            auto operation = ex::connect(std::move(sender), receiver{&values});
            ex::start(operation);

            THEN("the supplied environment enables exactly one transfer") {
                STATIC_REQUIRE(!ex::sender_in<decltype(sender)>);
                STATIC_REQUIRE(ex::sender_in<decltype(sender), environment>);
                CHECK(values == 1);
                CHECK(object.calls == 1);
            }
        }
    }
}
