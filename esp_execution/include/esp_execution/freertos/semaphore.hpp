// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_execution/freertos/chrono.hpp"

namespace esp::execution::freertos {

struct isr_release_result {
    bool released;
    bool task_woken;
};

class binary_semaphore {
public:
    binary_semaphore() pre(!xPortInIsrContext()) {
        handle_ = xSemaphoreCreateBinary();
        contract_assert(handle_ != nullptr);
    }

    ~binary_semaphore() {
        // GCC 16 ICEs with pre() on this noexcept body; keep the check here.
        contract_assert(!xPortInIsrContext());
        vSemaphoreDelete(handle_);
    }

    binary_semaphore(const binary_semaphore&) =
        delete ("copying would create multiple owners of one FreeRTOS semaphore");
    binary_semaphore(binary_semaphore&&) =
        delete ("ownership is fixed at construction; moved-from states are not supported");
    binary_semaphore& operator=(const binary_semaphore&) =
        delete ("copying would create multiple owners of one FreeRTOS semaphore");
    binary_semaphore& operator=(binary_semaphore&&) =
        delete ("ownership is fixed at construction; moved-from states are not supported");

    /** Waits indefinitely; returns false if FreeRTOS aborts the wait. */
    [[nodiscard]] bool acquire() noexcept pre(!xPortInIsrContext()) {
        return xSemaphoreTake(handle_, portMAX_DELAY) == pdTRUE;
    }

    [[nodiscard]] bool try_acquire() noexcept pre(!xPortInIsrContext()) {
        return xSemaphoreTake(handle_, 0) == pdTRUE;
    }

    /** Nonpositive waits do not block; positive waits round up to whole ticks. */
    [[nodiscard]] bool try_acquire_for(std::chrono::nanoseconds timeout) noexcept
        pre(!xPortInIsrContext()) {
        return xSemaphoreTake(handle_, to_ticks(timeout)) == pdTRUE;
    }

    [[nodiscard]] bool release() noexcept pre(!xPortInIsrContext()) {
        return xSemaphoreGive(handle_) == pdTRUE;
    }

    [[nodiscard]] auto release_from_isr() noexcept -> isr_release_result {
        // Same compiler workaround as the destructor; this is a caller precondition.
        contract_assert(xPortInIsrContext());
        BaseType_t task_woken = pdFALSE;
        const auto released = xSemaphoreGiveFromISR(handle_, &task_woken);
        return {.released = released == pdTRUE, .task_woken = task_woken != pdFALSE};
    }

private:
    SemaphoreHandle_t handle_ = nullptr;
};

} // namespace esp::execution::freertos
