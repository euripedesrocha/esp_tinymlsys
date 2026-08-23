// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>
#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

struct empty_env {};

struct get_env_t {
    constexpr decltype(auto)
    operator()(const detail::get_env_callable auto& object) const noexcept {
        if constexpr (detail::has_get_env_member<decltype(object)>) {
            return object.get_env();
        } else {
            return empty_env{};
        }
    }
};

inline constexpr get_env_t get_env{};

template <class Object>
using env_of_t = decltype(get_env(std::declval<const std::remove_cvref_t<Object>&>()));

} // namespace esp::execution
