// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <concepts>
#include <cstddef>
#include <optional>
#include <span>
#include <type_traits>

#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"

namespace esp::execution::io {
namespace detail {

template <class IO>
concept member_readable = requires(IO& object, std::span<std::byte> buffer) {
    { object.async_read_some(buffer) } -> sender;
};

template <class IO>
concept member_writable = requires(IO& object, std::span<const std::byte> buffer) {
    { object.async_write_some(buffer) } -> sender;
};

template <class Signature> inline constexpr bool byte_count_completion = false;

template <class Error>
inline constexpr bool byte_count_completion<set_value_t(std::optional<Error>, std::size_t)> =
    std::is_nothrow_move_constructible_v<Error>;

template <class Source, class Env>
concept byte_count_sender_in =
    sender_in<Source, Env> &&
    execution::detail::all_completion_signatures_satisfy_v<
        completion_signatures_of_t<Source, Env>, []<class Signature> {
            return !execution::detail::completion_signature_matches_tag_v<Signature, set_value_t> ||
                   byte_count_completion<Signature>;
        }>;

} // namespace detail

// Value completion reports an optional typed condition and an acknowledged
// byte count. An engaged condition can accompany a nonzero count.
struct async_read_some_t {
    auto operator()(sender auto source, detail::member_readable auto& object) const {
        return execution::let_value(std::move(source),
                                    [&object](std::span<std::byte> buffer) noexcept(
                                        noexcept(auto(object.async_read_some(buffer)))) {
                                        return object.async_read_some(buffer);
                                    });
    }

    auto operator()(detail::member_readable auto& object,
                    std::span<std::byte> buffer) const noexcept {
        return (*this)(execution::just(buffer), object);
    }

    auto operator()(detail::member_readable auto& object) const {
        return execution::detail::sender_adaptor_binding{
            [&object](auto source) { return async_read_some_t{}(std::move(source), object); }};
    }
};
inline constexpr async_read_some_t async_read_some{};

struct async_write_some_t {
    auto operator()(sender auto source, detail::member_writable auto& object) const {
        return execution::let_value(std::move(source),
                                    [&object](std::span<const std::byte> buffer) noexcept(
                                        noexcept(auto(object.async_write_some(buffer)))) {
                                        return object.async_write_some(buffer);
                                    });
    }

    auto operator()(detail::member_writable auto& object,
                    std::span<const std::byte> buffer) const noexcept {
        return (*this)(execution::just(buffer), object);
    }

    auto operator()(detail::member_writable auto& object) const {
        return execution::detail::sender_adaptor_binding{
            [&object](auto source) { return async_write_some_t{}(std::move(source), object); }};
    }
};
inline constexpr async_write_some_t async_write_some{};

template <class IO, class Env = empty_env>
concept readable = requires(IO& object, std::span<std::byte> buffer) {
    { io::async_read_some(object, buffer) } -> detail::byte_count_sender_in<Env>;
};

template <class IO, class Env = empty_env>
concept writable = requires(IO& object, std::span<const std::byte> buffer) {
    { io::async_write_some(object, buffer) } -> detail::byte_count_sender_in<Env>;
};

template <class IO, class Env = empty_env>
concept read_write = readable<IO, Env> && writable<IO, Env>;

} // namespace esp::execution::io
