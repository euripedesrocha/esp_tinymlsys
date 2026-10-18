// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/sender_adaptor.hpp"

namespace esp::execution::detail {

template <class Result> consteval auto continuation_result_signature() {
    if constexpr (std::is_void_v<Result>) {
        return std::type_identity<set_value_t()>{};
    } else {
        return std::type_identity<set_value_t(Result)>{};
    }
}

template <class Result>
using continuation_result_signature_t =
    typename decltype(continuation_result_signature<Result>())::type;

template <class Signature, class SelectedTag, class Callable> struct continuation_signature {
    using type = completion_signatures<Signature>;
};

template <class SelectedTag, class... Arguments, class Callable>
struct continuation_signature<SelectedTag(Arguments...), SelectedTag, Callable> {
    using type = completion_signatures<
        continuation_result_signature_t<std::invoke_result_t<Callable&&, Arguments...>>>;
};

template <class Signatures, class SelectedTag, class Callable> struct continuation_signatures;

template <class... Signatures, class SelectedTag, class Callable>
struct continuation_signatures<completion_signatures<Signatures...>, SelectedTag, Callable> {
    using type = completion_signatures_cat_t<
        typename continuation_signature<Signatures, SelectedTag, Callable>::type...>;
};

template <class Signatures, class SelectedTag, class Callable>
using continuation_signatures_t =
    typename continuation_signatures<Signatures, SelectedTag, Callable>::type;

template <class Sender, class SelectedTag, class Callable, class Env = empty_env>
concept continuation_for =
    sender_in<Sender, Env> &&
    all_selected_completion_signatures_satisfy_v<
        completion_signatures_of_t<Sender, Env>, SelectedTag,
        []<class... Arguments> { return std::is_nothrow_invocable_v<Callable&&, Arguments...>; }>;

template <class CompletionFunctionTag, class Receiver, class Callable> class continuation_receiver {
public:
    using receiver_concept = receiver_tag;

    constexpr continuation_receiver(Receiver receiver, Callable callable)
        : receiver_(std::move(receiver)), callable_(std::move(callable)) {}

    constexpr decltype(auto) get_env() const noexcept { return execution::get_env(receiver_); }

    constexpr void set_value(auto&&... values) && noexcept {
        std::move(*this).complete(set_value_t{}, std::forward<decltype(values)>(values)...);
    }

    constexpr void set_error(auto&& error) && noexcept {
        std::move(*this).complete(set_error_t{}, std::forward<decltype(error)>(error));
    }

    constexpr void set_stopped() && noexcept { std::move(*this).complete(set_stopped_t{}); }

private:
    // When to execute the continuation we invoke the callable and set value on the chain
    template <class... Arguments>
    constexpr void complete(CompletionFunctionTag, Arguments&&... arguments) && noexcept {
        if constexpr (std::is_void_v<std::invoke_result_t<Callable&&, Arguments...>>) {
            std::invoke(std::move(callable_), std::forward<Arguments>(arguments)...);
            execution::set_value(std::move(receiver_));
        } else {
            execution::set_value(
                std::move(receiver_),
                std::invoke(std::move(callable_), std::forward<Arguments>(arguments)...));
        }
    }

    // Once we are dealing with other completion function, we just forward the call.
    template <class Tag, class... Arguments>
        requires(!std::same_as<Tag, CompletionFunctionTag>)
    constexpr void complete(Tag tag, Arguments&&... arguments) && noexcept {
        tag(std::move(receiver_), std::forward<Arguments>(arguments)...);
    }

    Receiver receiver_;
    Callable callable_;
};

template <class SelectedTag, class Sender, movable_value Callable>
    requires continuation_for<Sender, SelectedTag, Callable>
class continuation_sender {
public:
    using sender_concept = sender_tag;

    template <class Env>
        requires std::same_as<Env, empty_env> ||
                     continuation_for<Sender, SelectedTag, Callable, Env>
    using completion_signatures =
        continuation_signatures_t<completion_signatures_of_t<Sender, Env>, SelectedTag, Callable>;

    constexpr continuation_sender(Sender sender, Callable callable)
        : sender_(std::move(sender)), callable_(std::move(callable)) {}

    constexpr decltype(auto) get_env() const noexcept { return execution::get_env(sender_); }

    constexpr auto connect(receiver_for_sender<continuation_sender> auto&& receiver) && {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return execution::connect(
            std::move(sender_),
            continuation_receiver<SelectedTag, receiver_type, Callable>{
                std::forward<decltype(receiver)>(receiver), std::move(callable_)});
    }

    constexpr auto connect(receiver_for_sender<continuation_sender> auto&& receiver) const&
        requires std::copy_constructible<Sender> && std::copy_constructible<Callable>
    {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return execution::connect(sender_,
                                  continuation_receiver<SelectedTag, receiver_type, Callable>{
                                      std::forward<decltype(receiver)>(receiver), callable_});
    }

private:
    Sender sender_;
    Callable callable_;
};

template <class SelectedTag> struct continuation_adaptor {
    constexpr auto operator()(sender auto&& source, movable_value auto&& callable) const
        -> continuation_sender<SelectedTag, std::decay_t<decltype(source)>,
                               std::decay_t<decltype(callable)>> {
        return {std::forward<decltype(source)>(source), std::forward<decltype(callable)>(callable)};
    }

    constexpr auto operator()(movable_value auto&& callable) const {
        return sender_adaptor_binding{
            std::bind_back(*this, std::forward<decltype(callable)>(callable))};
    }
};

} // namespace esp::execution::detail
