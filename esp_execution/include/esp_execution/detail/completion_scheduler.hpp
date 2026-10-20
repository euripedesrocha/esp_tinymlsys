// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <utility>

#include "esp_execution/scheduler.hpp"

namespace esp::execution::detail {

// Filter only completion-resource attributes; a general query-forwarding model
// is not provided. Keep unqueried environments, including references, unchanged.
template <class Env, class... HiddenTags> struct completion_scheduler_attributes {
    Env upstream;

    template <completion_tag Tag>
        requires((!std::same_as<Tag, HiddenTags>) && ...) && completion_scheduler_provider<Env, Tag>
    constexpr decltype(auto) query(get_completion_scheduler_t<Tag> tag) const noexcept {
        return std::as_const(upstream).query(tag);
    }
};

template <class... HiddenTags>
constexpr decltype(auto)
hide_completion_scheduler_attributes(const get_env_callable auto& source) noexcept {
    using Env = env_of_t<decltype(source)>;
    if constexpr ((completion_scheduler_provider<Env, HiddenTags> || ...)) {
        return completion_scheduler_attributes<Env, HiddenTags...>{execution::get_env(source)};
    } else {
        return execution::get_env(source);
    }
}

} // namespace esp::execution::detail
