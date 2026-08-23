// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <functional>
#include <type_traits>
#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

template <class Derived> struct sender_adaptor_closure {};

namespace detail {

template <class Type>
concept sender_adaptor_closure_type =
    std::derived_from<std::remove_cvref_t<Type>,
                      sender_adaptor_closure<std::remove_cvref_t<Type>>> &&
    (!sender<Type>);

template <class BoundCallable>
class sender_adaptor_binding
    : public sender_adaptor_closure<sender_adaptor_binding<BoundCallable>> {
public:
    constexpr explicit sender_adaptor_binding(BoundCallable callable)
        : callable_(std::move(callable)) {}

    constexpr decltype(auto) operator()(this auto&& self, sender auto&& sender) noexcept(
        std::is_nothrow_invocable_v<decltype(std::forward_like<decltype(self)>(self.callable_)),
                                    decltype(sender)>)
        requires std::invocable<decltype(std::forward_like<decltype(self)>(self.callable_)),
                                decltype(sender)>
    {
        return std::invoke(std::forward_like<decltype(self)>(self.callable_),
                           std::forward<decltype(sender)>(sender));
    }

private:
    BoundCallable callable_;
};

template <class Closure, class Sender>
concept adaptor_closure_for =
    sender_adaptor_closure_type<Closure> && requires(Closure&& closure, Sender&& sender) {
        { std::forward<Closure>(closure)(std::forward<Sender>(sender)) } -> esp::execution::sender;
    };

} // namespace detail

constexpr decltype(auto) operator|(sender auto&& sender,
                                   detail::adaptor_closure_for<decltype(sender)> auto&& closure) {
    return std::forward<decltype(closure)>(closure)(std::forward<decltype(sender)>(sender));
}

} // namespace esp::execution
