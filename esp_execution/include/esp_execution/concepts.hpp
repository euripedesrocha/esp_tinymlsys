// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/environment.hpp"

namespace esp::execution {

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
