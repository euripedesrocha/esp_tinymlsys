// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace esp::execution {

struct empty_env;
template <class... Signatures> struct completion_signatures;
struct set_value_t;
struct set_error_t;
struct set_stopped_t;

namespace detail {

// get_env falls back only when no member exists. A present customization must
// be noexcept and return a destructible environment.
template <class Object>
concept has_get_env_member =
    requires(const std::remove_cvref_t<Object>& object) { object.get_env(); };

template <class Object>
concept valid_get_env_member = requires(const std::remove_cvref_t<Object>& object) {
    { object.get_env() } noexcept;
    requires std::destructible<decltype(object.get_env())>;
};

template <class Object>
concept get_env_callable = (!has_get_env_member<Object>) || valid_get_env_member<Object>;

} // namespace detail

struct receiver_tag {};
struct operation_state_tag {};
struct sender_tag {};

template <class Receiver>
concept receiver =
    std::derived_from<typename std::remove_cvref_t<Receiver>::receiver_concept, receiver_tag> &&
    std::move_constructible<std::remove_cvref_t<Receiver>> && detail::get_env_callable<Receiver>;

template <class Operation>
concept operation_state =
    std::derived_from<typename std::remove_cvref_t<Operation>::operation_state_concept,
                      operation_state_tag> &&
    requires(Operation& operation) {
        { operation.start() } noexcept -> std::same_as<void>;
    };

template <class Sender>
concept sender =
    std::derived_from<typename std::remove_cvref_t<Sender>::sender_concept, sender_tag> &&
    std::move_constructible<std::remove_cvref_t<Sender>> &&
    std::constructible_from<std::remove_cvref_t<Sender>, Sender> &&
    detail::get_env_callable<Sender>;

template <class Sender, class Env = empty_env>
using completion_signatures_of_t =
    typename std::remove_cvref_t<Sender>::template completion_signatures<Env>;

namespace detail {

template <class Type>
concept movable_value = std::move_constructible<std::decay_t<Type>> &&
                        std::constructible_from<std::decay_t<Type>, Type> &&
                        (!std::is_array_v<std::remove_reference_t<Type>>);

template <class Type>
concept valid_completion_argument = std::is_object_v<Type> || std::is_reference_v<Type>;

template <class Signature> inline constexpr bool is_completion_signature = false;

template <class... Values>
inline constexpr bool is_completion_signature<set_value_t(Values...)> =
    (valid_completion_argument<Values> && ...);

template <class Error>
inline constexpr bool is_completion_signature<set_error_t(Error)> =
    valid_completion_argument<Error>;

template <> inline constexpr bool is_completion_signature<set_stopped_t()> = true;

template <class Type> inline constexpr bool is_completion_signatures = false;

template <class... Signatures>
inline constexpr bool is_completion_signatures<completion_signatures<Signatures...>> =
    (is_completion_signature<Signatures> && ...);

template <class Receiver, class... Values>
concept member_set_value_invocable =
    receiver<Receiver> && requires(Receiver&& receiver, Values&&... values) {
        {
            std::forward<Receiver>(receiver).set_value(std::forward<Values>(values)...)
        } noexcept -> std::same_as<void>;
    };

template <class Receiver, class Error>
concept member_set_error_invocable =
    receiver<Receiver> && requires(Receiver&& receiver, Error&& error) {
        {
            std::forward<Receiver>(receiver).set_error(std::forward<Error>(error))
        } noexcept -> std::same_as<void>;
    };

template <class Receiver>
concept member_set_stopped_invocable = receiver<Receiver> && requires(Receiver&& receiver) {
    { std::forward<Receiver>(receiver).set_stopped() } noexcept -> std::same_as<void>;
};

} // namespace detail

template <class Sender, class Env = empty_env>
concept sender_in = sender<Sender> && requires {
    typename completion_signatures_of_t<Sender, Env>;
    requires detail::is_completion_signatures<completion_signatures_of_t<Sender, Env>>;
};

} // namespace esp::execution
