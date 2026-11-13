// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

struct get_scheduler_t {
    constexpr decltype(auto)
    operator()(const detail::queryable_with<const get_scheduler_t> auto& env) const noexcept {
        static_assert(noexcept(env.query(*this)), "get_scheduler customization must not throw");
        static_assert(scheduler<decltype(env.query(*this))>,
                      "get_scheduler customization must return a scheduler");
        return env.query(*this);
    }
};

inline constexpr get_scheduler_t get_scheduler{};

template <class CompletionTag> struct get_completion_scheduler_t {
    constexpr decltype(auto) operator()(
        const detail::completion_scheduler_provider<CompletionTag> auto& env) const noexcept {
        return env.query(*this);
    }
};

template <class CompletionTag>
inline constexpr get_completion_scheduler_t<CompletionTag> get_completion_scheduler{};

struct schedule_t {
    constexpr decltype(auto) operator()(scheduler auto&& scheduler) const
        noexcept(noexcept(std::forward<decltype(scheduler)>(scheduler).schedule())) {
        return std::forward<decltype(scheduler)>(scheduler).schedule();
    }
};

inline constexpr schedule_t schedule{};

template <scheduler Scheduler>
using schedule_result_t = decltype(schedule(std::declval<Scheduler>()));

} // namespace esp::execution
