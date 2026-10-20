// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution {

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
