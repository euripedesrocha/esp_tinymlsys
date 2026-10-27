// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_freertos_cpp/semaphore.hpp"

namespace esp_freertos_cpp_test {

inline void release(esp::freertos::binary_semaphore& semaphore) noexcept {
    const bool released = semaphore.release();
    contract_assert(released);
}

inline void notify_completion(esp::freertos::binary_semaphore& semaphore) noexcept {
    if (xPortInIsrContext()) {
        const auto result = semaphore.release_from_isr();
        contract_assert(result.released);
        if (result.task_woken)
            portYIELD_FROM_ISR(pdTRUE);
    } else {
        release(semaphore);
    }
}

// Pending operations cannot be cancelled. A timeout must not unwind borrowed storage.
inline void wait_for_completion(esp::freertos::binary_semaphore& semaphore) {
    if (!semaphore.try_acquire_for(std::chrono::seconds{5})) {
        std::fputs("Timed out waiting for completion; cannot safely unwind\n", stderr);
        std::abort();
    }
}

// A signal can arrive before its release call returns. Wait until the task no
// longer uses borrowed state, then delete its still-valid suspended handle.
inline void join_suspended_task(TaskHandle_t task) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != eSuspended) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000))
            std::abort();
        vTaskDelay(TickType_t{1});
    }
    vTaskDelete(task);
}

} // namespace esp_freertos_cpp_test
