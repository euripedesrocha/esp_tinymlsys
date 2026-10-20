// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "deferred_sender_test_support.hpp"
#include "esp_execution/scheduler.hpp"

namespace esp_execution_test {

struct manual_scheduler {
    using scheduler_concept = esp::execution::scheduler_tag;
    deferred_completion* pending;

    struct schedule_sender : deferred_sender {
        struct environment {
            deferred_completion* pending;

            auto query(esp::execution::get_completion_scheduler_t<esp::execution::set_value_t>)
                const noexcept -> manual_scheduler {
                return {pending};
            }
        };

        auto get_env() const noexcept -> environment { return {pending}; }
    };

    auto schedule() const noexcept { return schedule_sender{{.pending = pending}}; }
    friend bool operator==(manual_scheduler, manual_scheduler) noexcept = default;
};

} // namespace esp_execution_test
