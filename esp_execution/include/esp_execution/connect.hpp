// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

namespace detail {

template <class Sender, class Receiver>
concept member_connectable =
    sender<Sender> && receiver<Receiver> && requires(Sender&& sender, Receiver&& receiver) {
        {
            std::forward<Sender>(sender).connect(std::forward<Receiver>(receiver))
        } -> operation_state;
    };

template <class Sender, class Receiver>
inline constexpr bool is_nothrow_member_connect_v =
    member_connectable<Sender, Receiver> && requires(Sender&& sender, Receiver&& receiver) {
        { std::forward<Sender>(sender).connect(std::forward<Receiver>(receiver)) } noexcept;
    };

} // namespace detail

struct connect_t {
    constexpr auto operator()(sender auto&& sender, receiver auto&& receiver) const
        noexcept(detail::is_nothrow_member_connect_v<decltype(sender), decltype(receiver)>)
        requires detail::member_connectable<decltype(sender), decltype(receiver)>
    {
        return std::forward<decltype(sender)>(sender).connect(
            std::forward<decltype(receiver)>(receiver));
    }
};

inline constexpr connect_t connect{};

} // namespace esp::execution
