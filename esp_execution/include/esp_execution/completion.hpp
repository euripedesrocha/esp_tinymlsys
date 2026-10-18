// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

struct set_value_t {
    constexpr void operator()(receiver auto&& receiver, auto&&... values) const noexcept
        requires detail::member_set_value_invocable<decltype(receiver), decltype(values)...>
    {
        std::forward<decltype(receiver)>(receiver).set_value(
            std::forward<decltype(values)>(values)...);
    }
};

struct set_error_t {
    constexpr void operator()(receiver auto&& receiver, auto&& error) const noexcept
        requires detail::member_set_error_invocable<decltype(receiver), decltype(error)>
    {
        std::forward<decltype(receiver)>(receiver).set_error(std::forward<decltype(error)>(error));
    }
};

struct set_stopped_t {
    constexpr void operator()(detail::member_set_stopped_invocable auto&& receiver) const noexcept {
        std::forward<decltype(receiver)>(receiver).set_stopped();
    }
};

inline constexpr set_value_t set_value{};
inline constexpr set_error_t set_error{};
inline constexpr set_stopped_t set_stopped{};

} // namespace esp::execution
