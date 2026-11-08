// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <exception>
#include <functional>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_scheduler.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/operation_state.hpp"
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

template <class Sender, class SelectedTag, class Callable, class Env = empty_env>
concept continuation_for =
    sender_in<Sender, Env> &&
    all_selected_completion_signatures_satisfy_v<
        completion_signatures_of_t<Sender, Env>, SelectedTag,
        []<class... Arguments> { return std::is_invocable_v<Callable&&, Arguments...>; }>;

template <class CompletionFunctionTag, class Sender, class Receiver, class Callable>
class continuation_operation {
    Receiver receiver_;
    Callable callable_;

    struct upstream_receiver {
        using receiver_concept = receiver_tag;
        continuation_operation* parent;

        constexpr decltype(auto) get_env() const noexcept {
            return execution::get_env(parent->receiver_);
        }
        constexpr void set_value(auto&&... values) && noexcept {
            std::move(*parent).complete(set_value_t{}, std::forward<decltype(values)>(values)...);
        }
        constexpr void set_error(auto&& error) && noexcept {
            std::move(*parent).complete(set_error_t{}, std::forward<decltype(error)>(error));
        }
        constexpr void set_stopped() && noexcept { std::move(*parent).complete(set_stopped_t{}); }
    };

    using upstream_operation =
        decltype(execution::connect(std::declval<Sender>(), upstream_receiver{}));
    upstream_operation upstream_;

public:
    using operation_state_concept = operation_state_tag;

    constexpr continuation_operation(
        execution::sender auto&& source, execution::receiver auto&& receiver,
        movable_value auto&&
            callable) noexcept(std::is_nothrow_constructible_v<Receiver, decltype(receiver)> &&
                               std::is_nothrow_constructible_v<Callable, decltype(callable)> &&
                               noexcept(execution::connect(std::forward<decltype(source)>(source),
                                                           upstream_receiver{})))
        : receiver_(std::forward<decltype(receiver)>(receiver)),
          callable_(std::forward<decltype(callable)>(callable)),
          upstream_(execution::connect(std::forward<decltype(source)>(source),
                                       upstream_receiver{this})) {}

    continuation_operation(const continuation_operation&) =
        delete ("the operation owns its receiver and callable");
    continuation_operation(continuation_operation&&) =
        delete ("the upstream receiver retains this operation's address");
    continuation_operation& operator=(const continuation_operation&) =
        delete ("the operation owns its receiver and callable");
    continuation_operation& operator=(continuation_operation&&) =
        delete ("the upstream receiver retains this operation's address");

    constexpr void start() & noexcept { execution::start(upstream_); }

private:
    template <class... Arguments>
    constexpr void complete(CompletionFunctionTag, Arguments&&... arguments) && noexcept {
        constexpr bool nothrow = std::is_nothrow_invocable_v<Callable&&, Arguments...>;
        auto invoke = [&]() noexcept(nothrow) {
            if constexpr (std::is_void_v<std::invoke_result_t<Callable&&, Arguments...>>) {
                std::invoke(std::move(callable_), std::forward<Arguments>(arguments)...);
                execution::set_value(std::move(receiver_));
            } else {
                execution::set_value(
                    std::move(receiver_),
                    std::invoke(std::move(callable_), std::forward<Arguments>(arguments)...));
            }
        };
        if constexpr (nothrow) {
            invoke();
        } else {
            try {
                invoke();
            } catch (...) {
                execution::set_error(std::move(receiver_), std::current_exception());
            }
        }
    }

    template <class Tag, class... Arguments>
        requires(!std::same_as<Tag, CompletionFunctionTag>)
    constexpr void complete(Tag tag, Arguments&&... arguments) && noexcept {
        tag(std::move(receiver_), std::forward<Arguments>(arguments)...);
    }
};

template <class SelectedTag, execution::sender Sender, movable_value Callable>
class continuation_sender {
public:
    using sender_concept = sender_tag;

    template <class Env>
        requires continuation_for<Sender, SelectedTag, Callable, Env>
    using completion_signatures = map_completion_signatures_t<
        completion_signatures_of_t<Sender, Env>, []<class Tag, class... Arguments> {
            if constexpr (std::same_as<Tag, SelectedTag>) {
                using result =
                    continuation_result_signature_t<std::invoke_result_t<Callable&&, Arguments...>>;
                using errors = std::conditional_t<
                    std::is_nothrow_invocable_v<Callable&&, Arguments...>,
                    execution::completion_signatures<>,
                    execution::completion_signatures<set_error_t(std::exception_ptr)>>;
                return std::type_identity<completion_signatures_cat_t<
                    execution::completion_signatures<result>, errors>>{};
            } else {
                return std::type_identity<execution::completion_signatures<Tag(Arguments...)>>{};
            }
        }>;

    constexpr continuation_sender(Sender sender, Callable callable)
        : sender_(std::move(sender)), callable_(std::move(callable)) {}

    constexpr decltype(auto) get_env() const noexcept {
        if constexpr (std::same_as<SelectedTag, set_value_t>) {
            return execution::get_env(sender_);
        } else {
            // Recovering a terminal channel can produce a value on another resource.
            return hide_completion_scheduler_attributes<set_value_t, SelectedTag>(sender_);
        }
    }

    constexpr auto
    connect(receiver_for_sender<continuation_sender> auto&& receiver) && noexcept(noexcept(
        continuation_operation<SelectedTag, Sender, std::decay_t<decltype(receiver)>, Callable>{
            std::declval<Sender>(), std::forward<decltype(receiver)>(receiver),
            std::declval<Callable>()})) {
        return continuation_operation<SelectedTag, Sender, std::decay_t<decltype(receiver)>,
                                      Callable>{
            std::move(sender_), std::forward<decltype(receiver)>(receiver), std::move(callable_)};
    }

    constexpr auto connect(receiver_for_sender<continuation_sender> auto&& receiver)
        const& noexcept(noexcept(continuation_operation<SelectedTag, const Sender&,
                                                        std::decay_t<decltype(receiver)>, Callable>{
            std::declval<const Sender&>(), std::forward<decltype(receiver)>(receiver),
            std::declval<const Callable&>()}))
        requires std::copy_constructible<Sender> && std::copy_constructible<Callable>
    {
        return continuation_operation<SelectedTag, const Sender&, std::decay_t<decltype(receiver)>,
                                      Callable>{sender_, std::forward<decltype(receiver)>(receiver),
                                                callable_};
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
