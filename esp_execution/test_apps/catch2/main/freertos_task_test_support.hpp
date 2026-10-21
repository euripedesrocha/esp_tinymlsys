// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_execution.hpp"
#include "esp_execution/freertos/task_context.hpp"

namespace esp_execution_test {

namespace execution = esp::execution;
using execution::freertos::schedule_error;

struct completion_record {
    TaskHandle_t waiter = nullptr;
    std::atomic<TaskHandle_t> completion_task = nullptr;
    std::atomic_int values = 0;
    std::atomic_int errors = 0;
    std::atomic<schedule_error> error = schedule_error::queue_full;
    std::atomic_int stopped = 0;
    std::atomic_int value = 0;
    std::atomic_int order = 0;
    std::atomic_int* sequence = nullptr;
};

struct recording_receiver {
    using receiver_concept = execution::receiver_tag;

    completion_record* record;

    auto get_env() const noexcept -> execution::empty_env { return {}; }

    void set_value() && noexcept { complete_value(0); }
    void set_value(int value) && noexcept { complete_value(value); }

    void set_error(schedule_error error) const&& noexcept {
        record->completion_task.store(xTaskGetCurrentTaskHandle());
        record->error.store(error);
        record->errors.fetch_add(1);
        xTaskNotifyGive(record->waiter);
    }

    void set_stopped() const&& noexcept {
        record->completion_task.store(xTaskGetCurrentTaskHandle());
        record->stopped.fetch_add(1);
        xTaskNotifyGive(record->waiter);
    }

private:
    void complete_value(int value) const noexcept {
        record->completion_task.store(xTaskGetCurrentTaskHandle());
        record->value.store(value);
        if (record->sequence != nullptr) {
            record->order.store(record->sequence->fetch_add(1) + 1);
        }
        record->values.fetch_add(1);
        xTaskNotifyGive(record->waiter);
    }
};

// These operations cannot be cancelled. On timeout, fail-stop rather than unwind
// storage whose address may still be queued or in use by a worker.
inline void wait_for_notifications(std::uint32_t expected) {
    std::uint32_t received = 0;
    while (received < expected) {
        const auto count = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000));
        if (count == 0) {
            std::fputs("Timed out waiting for task completion; cannot safely unwind\n", stderr);
            std::abort();
        }
        received += count;
    }
}

inline void clear_notifications() {
    while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
    }
}

} // namespace esp_execution_test
