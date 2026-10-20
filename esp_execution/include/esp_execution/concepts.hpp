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
struct scheduler_tag {};

template <class CompletionTag> struct get_completion_scheduler_t;

namespace detail {

template <class Type>
concept movable_value = std::move_constructible<std::decay_t<Type>> &&
                        std::constructible_from<std::decay_t<Type>, Type> &&
                        (!std::is_array_v<std::remove_reference_t<Type>>);

template <class CompletionTag>
concept completion_tag =
    std::same_as<CompletionTag, set_value_t> || std::same_as<CompletionTag, set_error_t> ||
    std::same_as<CompletionTag, set_stopped_t>;

} // namespace detail

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

template <class Sender, class Env = empty_env>
concept sender_in = sender<Sender> && requires {
    typename completion_signatures_of_t<Sender, Env>;
    requires detail::is_completion_signatures<completion_signatures_of_t<Sender, Env>>;
};

namespace detail {

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

template <class Receiver, class Signature> inline constexpr bool accepts_completion_v = false;

template <class Receiver, class... Values>
inline constexpr bool accepts_completion_v<Receiver, set_value_t(Values...)> =
    member_set_value_invocable<Receiver, Values...>;

template <class Receiver, class Error>
inline constexpr bool accepts_completion_v<Receiver, set_error_t(Error)> =
    member_set_error_invocable<Receiver, Error>;

template <class Receiver>
inline constexpr bool accepts_completion_v<Receiver, set_stopped_t()> =
    member_set_stopped_invocable<Receiver>;

template <class Receiver, class Signatures>
concept receiver_for =
    receiver<Receiver> && all_completion_signatures_satisfy_v<Signatures, []<class Signature> {
        return accepts_completion_v<Receiver, Signature>;
    }>;

// connect owns its receiver: copy or move the argument into storage, then
// deliver completions to that stored receiver as an rvalue.
template <class Receiver, class Sender>
concept receiver_for_sender =
    std::constructible_from<std::decay_t<Receiver>, Receiver> &&
    receiver_for<std::decay_t<Receiver>, completion_signatures_of_t<Sender, env_of_t<Receiver>>>;

template <class Scheduler>
concept member_schedule_invocable = requires(Scheduler&& scheduler) {
    { std::forward<Scheduler>(scheduler).schedule() } -> sender;
};

} // namespace detail

template <class Scheduler>
concept scheduler =
    std::derived_from<typename std::remove_cvref_t<Scheduler>::scheduler_concept, scheduler_tag> &&
    std::copy_constructible<std::remove_cvref_t<Scheduler>> &&
    std::equality_comparable<std::remove_cvref_t<Scheduler>> &&
    detail::member_schedule_invocable<Scheduler> &&
    requires(
        const std::remove_reference_t<env_of_t<decltype(std::declval<Scheduler>().schedule())>>&
            env) {
        {
            auto(env.query(std::declval<get_completion_scheduler_t<set_value_t>>()))
        } noexcept -> std::same_as<std::remove_cvref_t<Scheduler>>;
    };

namespace detail {

template <class Env, class CompletionTag>
concept completion_scheduler_provider =
    completion_tag<CompletionTag> && requires(const std::remove_reference_t<Env>& env) {
        {
            env.query(std::declval<get_completion_scheduler_t<CompletionTag>>())
        } noexcept -> scheduler;
    };

} // namespace detail

} // namespace esp::execution
