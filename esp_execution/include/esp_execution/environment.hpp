// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <type_traits>
#include <utility>

namespace esp::execution {

struct empty_env {};

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
