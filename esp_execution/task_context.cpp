// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/freertos/task_context.hpp"

#include <cstdlib>

namespace esp::execution::freertos {

task_context::task_context(config configuration) {
    contract_assert(!xPortInIsrContext());
    queue_ = xQueueCreate(configuration.queue_capacity, sizeof(detail::task_job));
    contract_assert(queue_ != nullptr);
    completed_ = xSemaphoreCreateBinary();
    contract_assert(completed_ != nullptr);

    const auto created =
        xTaskCreatePinnedToCore(task_entry, configuration.name, configuration.stack_size, this,
                                configuration.priority, &task_, configuration.core_id);
    contract_assert(created == pdPASS);
}

task_context::~task_context() {
    finish();
    xSemaphoreTake(completed_, portMAX_DELAY);
    vSemaphoreDelete(completed_);
    vQueueDelete(queue_);
}

void task_context::finish() noexcept {
    contract_assert(!xPortInIsrContext());
    contract_assert(task_ != xTaskGetCurrentTaskHandle());
    if (!accepting_.exchange(false)) {
        return;
    }

    const detail::task_job stop{};
    if (xQueueSend(queue_, &stop, portMAX_DELAY) != pdPASS) {
        std::abort();
    }
}

auto task_context::submit(detail::task_job job, submission_policy policy) noexcept
    -> detail::submission_result {
    const bool from_isr = xPortInIsrContext() == pdTRUE;
    if (policy.wait_ticks_ != 0) {
        contract_assert(!from_isr);
        contract_assert(task_ != xTaskGetCurrentTaskHandle());
    }

    if (!accepting_.load()) {
        return detail::submission_result::stopped;
    }

    if (from_isr) {
        BaseType_t task_woken = pdFALSE;
        const auto queued = xQueueSendFromISR(queue_, &job, &task_woken);
        if (task_woken != pdFALSE) {
            portYIELD_FROM_ISR(task_woken);
        }
        return queued == pdPASS ? detail::submission_result::accepted
                                : detail::submission_result::queue_full;
    }
    if (xQueueSend(queue_, &job, policy.wait_ticks_) != pdPASS) {
        return policy.wait_ticks_ == 0 ? detail::submission_result::queue_full
                                       : detail::submission_result::queue_timeout;
    }

    return detail::submission_result::accepted;
}

void task_context::run() noexcept {
    detail::task_job job;
    while (xQueueReceive(queue_, &job, portMAX_DELAY) == pdPASS) {
        if (job.complete == nullptr) {
            return;
        }
        job.complete(job.state);
    }
}

void task_context::task_entry(void* state) noexcept {
    auto& self = *static_cast<task_context*>(state);
    self.run();
    xSemaphoreGive(self.completed_);
    vTaskDelete(nullptr);
}

} // namespace esp::execution::freertos
