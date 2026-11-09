// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/io/primitives.hpp"
#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <optional>
#include <system_error>

namespace {
namespace ex = esp::execution;
using count_sender = decltype(ex::just(std::optional<std::error_code>{}, std::size_t{}));

struct input {
    input(const input&) = delete;
    count_sender async_read_some(std::span<std::byte>);
};
struct output {
    count_sender async_write_some(std::span<const std::byte>);
};
struct count_only {
    auto async_read_some(std::span<std::byte>) { return ex::just(std::size_t{}); }
};
struct throwing_error {
    throwing_error() = default;
    throwing_error(throwing_error&&) noexcept(false);
};
struct throwing_condition {
    using source = decltype(ex::just(std::optional<throwing_error>{}, std::size_t{}));
    source async_read_some(std::span<std::byte>);
};
struct io_env {};
struct environment_sender {
    using sender_concept = ex::sender_tag;
    template <class Env>
        requires std::same_as<Env, io_env>
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(std::optional<std::error_code>, std::size_t)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
};
struct environment_io {
    auto async_read_some(std::span<std::byte>) { return environment_sender{}; }
    auto async_write_some(std::span<const std::byte>) { return environment_sender{}; }
};
struct non_sender {
    int async_read_some(std::span<std::byte>);
    int async_write_some(std::span<const std::byte>);
};

TEST_CASE("I/O concepts describe asynchronous byte capabilities", "[io][concepts]") {
    STATIC_REQUIRE(ex::io::readable<input> && !ex::io::writable<input>);
    STATIC_REQUIRE(ex::io::writable<output> && !ex::io::readable<output>);
    STATIC_REQUIRE(!ex::io::readable<count_only>);
    STATIC_REQUIRE(!ex::io::readable<throwing_condition>);
    STATIC_REQUIRE(!ex::io::readable<environment_io>);
    STATIC_REQUIRE(ex::io::read_write<environment_io, io_env>);
    STATIC_REQUIRE(
        !std::invocable<decltype(ex::io::async_read_some), non_sender&, std::span<std::byte>>);
    STATIC_REQUIRE(!std::invocable<decltype(ex::io::async_write_some), non_sender&,
                                   std::span<const std::byte>>);
}
} // namespace
