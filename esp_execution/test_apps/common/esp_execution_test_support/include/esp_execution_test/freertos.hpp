// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <contracts>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_execution.hpp"
#include "esp_freertos_cpp_test/support.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "esp_execution/freertos/wait.hpp"

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

namespace wait_support {

namespace execution = esp::execution;
using esp::freertos::binary_semaphore;

inline void wait_until_blocked(TaskHandle_t task) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != eBlocked) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000)) {
            std::fputs("wait test: task did not block\n", stderr);
            std::abort();
        }
        vTaskDelay(TickType_t{1});
    }
}

struct pending_completion {
    binary_semaphore ready;
    TaskHandle_t waiter = nullptr;
    void* operation = nullptr;
    void (*complete)(void*) noexcept = nullptr;
    std::atomic_bool destroyed = false;
    // External test storage: keep a producer callback active after wait has
    // returned and destroyed its operation, receiver, result, and semaphore.
    std::atomic_bool* completion_return_gate = nullptr;
    bool stop = false;
};

// The producer keeps only an address until completion, just like an ISR source.
struct deferred_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(int), execution::set_stopped_t()>;
    pending_completion& pending;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        pending_completion& pending;
        Receiver receiver;

        operation(pending_completion& pending, Receiver receiver)
            : pending(pending), receiver(std::move(receiver)) {}
        operation(const operation&) = delete;
        operation(operation&&) = delete;
        ~operation() { pending.destroyed.store(true); }

        void start() & noexcept {
            pending.waiter = xTaskGetCurrentTaskHandle();
            pending.operation = this;
            pending.complete = [](void* address) noexcept {
                auto& self = *static_cast<operation*>(address);
                auto* gate = self.pending.completion_return_gate;
                if (self.pending.stop)
                    execution::set_stopped(std::move(self.receiver));
                else
                    execution::set_value(std::move(self.receiver), 42);
                // The waiter can destroy this operation during completion.
                // Only the independently owned gate may be used afterward.
                if (gate) {
                    const auto tick = []() noexcept {
                        return xPortInIsrContext() ? xTaskGetTickCountFromISR()
                                                   : xTaskGetTickCount();
                    };
                    const auto started = tick();
                    while (!gate->load()) {
                        if (tick() - started >= pdMS_TO_TICKS(5000)) {
                            std::fputs("wait test: remote waiter did not return\n", stderr);
                            std::abort();
                        }
                    }
                }
            };
            esp_freertos_cpp_test::release(pending.ready);
        }
    };

    auto connect(execution::detail::receiver_for_sender<deferred_sender> auto&& receiver) const {
        return operation<std::decay_t<decltype(receiver)>>{
            pending, std::forward<decltype(receiver)>(receiver)};
    }
};

#if !CONFIG_FREERTOS_UNICORE
struct remote_completion_result {
    bool value_correct = false;
    bool returned = false;
    bool destroyed = false;
    BaseType_t waiter_core = -1;
    BaseType_t completion_core = -1;
    bool completion_from_isr = false;
};

// The dispatcher runs on core 1 and invokes complete either there or in its
// interrupt. The completion stays active until the core-0 waiter has returned.
template <class Dispatch>
auto complete_on_other_core(bool stopped, Dispatch dispatch) -> remote_completion_result {
    pending_completion pending;
    std::atomic_bool returned = false;
    pending.stop = stopped;
    pending.completion_return_gate = &returned;
    binary_semaphore waiter_done;
    binary_semaphore producer_done;
    remote_completion_result result;
    struct fixture {
        pending_completion& pending;
        std::atomic_bool& returned;
        binary_semaphore& waiter_done;
        binary_semaphore& producer_done;
        Dispatch& dispatch;
        remote_completion_result& result;
    } state{pending, returned, waiter_done, producer_done, dispatch, result};
    TaskHandle_t waiter = nullptr;
    const auto waiter_created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& state = *static_cast<fixture*>(argument);
            state.result.waiter_core = xPortGetCoreID();
            const auto value = execution::freertos::wait(deferred_sender{state.pending});
            state.result.value_correct =
                state.pending.stop ? !value : value && std::get<0>(*value) == 42;
            // wait's operation and semaphore are already destroyed.
            state.returned.store(true);
            esp_freertos_cpp_test::release(state.waiter_done);
            vTaskSuspend(nullptr);
        },
        "remote_waiter", 4096, &state, tskIDLE_PRIORITY + 3, &waiter, 0);
    if (waiter_created != pdPASS)
        std::abort();
    TaskHandle_t producer = nullptr;
    const auto producer_created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& state = *static_cast<fixture*>(argument);
            esp_freertos_cpp_test::wait_for_completion(state.pending.ready);
            wait_until_blocked(state.pending.waiter);
            const auto complete = [&]() noexcept {
                state.result.completion_core = xPortGetCoreID();
                state.result.completion_from_isr = xPortInIsrContext();
                state.pending.complete(state.pending.operation);
            };
            state.dispatch(complete);
            esp_freertos_cpp_test::release(state.producer_done);
            vTaskSuspend(nullptr);
        },
        "remote_producer", 4096, &state, tskIDLE_PRIORITY + 3, &producer, 1);
    // A live waiter borrows this fixture, so creation failure cannot unwind it.
    if (producer_created != pdPASS)
        std::abort();
    esp_freertos_cpp_test::wait_for_completion(waiter_done);
    esp_freertos_cpp_test::wait_for_completion(producer_done);
    esp_freertos_cpp_test::join_suspended_task(waiter);
    esp_freertos_cpp_test::join_suspended_task(producer);
    result.returned = returned.load();
    result.destroyed = pending.destroyed.load();
    return result;
}
#endif

} // namespace wait_support

} // namespace esp_execution_test
