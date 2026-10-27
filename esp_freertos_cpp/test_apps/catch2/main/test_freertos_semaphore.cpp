// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "esp_freertos_cpp/semaphore.hpp"

#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_GPTIMER_ISR_CACHE_SAFE
#include <driver/gptimer.h>
#endif

namespace {

using esp::freertos::binary_semaphore;
using namespace std::chrono_literals;

void clear_notifications() {
    while (ulTaskNotifyTake(pdTRUE, 0) != 0) {
    }
}

void wait_for_task() {
    // A timed-out task can still borrow stack storage. Do not unwind that storage.
    if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(5000)) == 0) {
        std::fputs("Semaphore test task timed out; cannot safely unwind\n", stderr);
        std::abort();
    }
}

void wait_until_blocked(TaskHandle_t task) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != eBlocked) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000)) {
            std::fputs("Semaphore test task did not block; cannot safely unwind\n", stderr);
            std::abort();
        }
        vTaskDelay(TickType_t{1});
    }
}

enum class acquisition : uint8_t { indefinite, timed };

} // namespace

TEST_CASE("FreeRTOS binary semaphore acquisition and release", "[freertos][semaphore]") {
    binary_semaphore semaphore;

    SECTION("a new semaphore is empty") { REQUIRE_FALSE(semaphore.try_acquire()); }

    SECTION("release makes exactly one token available") {
        REQUIRE(semaphore.release());
        REQUIRE(semaphore.try_acquire());
        REQUIRE_FALSE(semaphore.try_acquire());
    }

    SECTION("release fails when full without adding another token") {
        REQUIRE(semaphore.release());
        REQUIRE_FALSE(semaphore.release());
        REQUIRE(semaphore.try_acquire());
        REQUIRE_FALSE(semaphore.try_acquire());
    }

    SECTION("acquisition permits the semaphore to be released and acquired again") {
        REQUIRE(semaphore.release());
        REQUIRE(semaphore.acquire());
        REQUIRE(semaphore.release());
        REQUIRE(semaphore.try_acquire());
        REQUIRE_FALSE(semaphore.try_acquire());
    }
}

TEST_CASE("nonpositive FreeRTOS semaphore timeouts still attempt acquisition",
          "[freertos][semaphore][timeout]") {
    const auto timeout = GENERATE(0ns, std::chrono::nanoseconds{-1ms});
    binary_semaphore semaphore;
    REQUIRE_FALSE(semaphore.try_acquire_for(timeout));
    REQUIRE(semaphore.release());
    REQUIRE(semaphore.try_acquire_for(timeout));
    REQUIRE_FALSE(semaphore.try_acquire());
}

TEST_CASE("FreeRTOS semaphore timeouts round positive durations up to ticks",
          "[freertos][semaphore][timeout]") {
    const auto timeout =
        GENERATE(1ns, std::chrono::nanoseconds{1ms}, std::chrono::nanoseconds{15ms});
    binary_semaphore semaphore;
    const auto started = xTaskGetTickCount();
    const bool acquired = semaphore.try_acquire_for(timeout);
    const auto elapsed = xTaskGetTickCount() - started;
    // Independent ceiling calculation exercises sub-tick and fractional-tick waits.
    const auto expected_ticks =
        (timeout.count() * configTICK_RATE_HZ + 999'999'999) / 1'000'000'000;
    REQUIRE_FALSE(acquired);
    REQUIRE(elapsed >= expected_ticks);
    REQUIRE(semaphore.release());
    REQUIRE(semaphore.try_acquire_for(1ns));
}

TEST_CASE("a FreeRTOS semaphore release unblocks another task", "[freertos][semaphore][tasks]") {
    const auto mode = GENERATE(acquisition::indefinite, acquisition::timed);
    clear_notifications();
    binary_semaphore semaphore;
    struct waiter_state {
        binary_semaphore* semaphore;
        TaskHandle_t caller;
        acquisition mode;
        int payload = 0;
        int observed = 0;
        bool acquired = false;
    } state{&semaphore, xTaskGetCurrentTaskHandle(), mode};
    TaskHandle_t waiter = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<waiter_state*>(argument);
            const auto caller = state.caller;
            if (state.mode == acquisition::indefinite) {
                state.acquired = state.semaphore->acquire();
            } else {
                state.acquired = state.semaphore->try_acquire_for(5s);
            }
            if (state.acquired)
                state.observed = state.payload;
            xTaskNotifyGive(caller);
            vTaskSuspend(nullptr);
        },
        "semaphore_waiter", 4096, &state, tskIDLE_PRIORITY + 1, &waiter);
    REQUIRE(created == pdPASS); // No task borrows test storage on failure.
    wait_until_blocked(waiter);
    state.payload = 42;
    const bool released = semaphore.release();
    wait_for_task();
    vTaskDelete(waiter);

    REQUIRE(released);
    REQUIRE(state.acquired);
    REQUIRE(state.observed == 42);
    REQUIRE_FALSE(semaphore.try_acquire());
}

TEST_CASE("aborting a FreeRTOS semaphore wait reports failure and leaves it usable",
          "[freertos][semaphore][tasks][abort]") {
    clear_notifications();
    binary_semaphore semaphore;
    struct waiter_state {
        binary_semaphore* semaphore;
        TaskHandle_t caller;
        bool acquired = true;
    } state{.semaphore = &semaphore, .caller = xTaskGetCurrentTaskHandle()};
    TaskHandle_t waiter = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<waiter_state*>(argument);
            const auto caller = state.caller;
            state.acquired = state.semaphore->acquire();
            xTaskNotifyGive(caller);
            vTaskSuspend(nullptr);
        },
        "aborted_sem_waiter", 4096, &state, tskIDLE_PRIORITY + 1, &waiter);
    REQUIRE(created == pdPASS); // No task borrows test storage on failure.
    wait_until_blocked(waiter);
    const auto aborted = xTaskAbortDelay(waiter);
    wait_for_task();
    vTaskDelete(waiter);

    REQUIRE(aborted == pdPASS);
    REQUIRE_FALSE(state.acquired);
    REQUIRE_FALSE(semaphore.try_acquire());
    REQUIRE(semaphore.release());
    REQUIRE(semaphore.acquire());
    REQUIRE_FALSE(semaphore.try_acquire());
}

// This test's native GPTimer callback is not placed in IRAM.
#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_GPTIMER_ISR_CACHE_SAFE
namespace {

using esp::freertos::isr_release_result;

enum class isr_scenario : uint8_t { empty, full, waiting_task };

struct isr_probe {
    isr_scenario scenario;
    TaskHandle_t caller;
    BaseType_t core;
    isr_release_result result{};
    bool initial_release = false;
    bool waiter_acquired = false;
    bool worker_ran_before_resume = false;
    bool token_remaining = false;
};

void check_driver(esp_err_t result) noexcept { contract_assert(result == ESP_OK); }

void run_isr_probe(void* argument) {
    auto& probe = *static_cast<isr_probe*>(argument);
    auto* const caller = probe.caller;
    {
        binary_semaphore semaphore;
        struct waiter_state {
            binary_semaphore* semaphore;
            TaskHandle_t caller;
            std::atomic_bool acquired = false;
        } waiter_data{&semaphore, xTaskGetCurrentTaskHandle()};
        TaskHandle_t waiter = nullptr;
        if (probe.scenario == isr_scenario::waiting_task) {
            const auto created = xTaskCreatePinnedToCore(
                [](void* argument) {
                    auto& state = *static_cast<waiter_state*>(argument);
                    const auto caller = state.caller;
                    state.acquired.store(state.semaphore->acquire());
                    xTaskNotifyGive(caller);
                    vTaskSuspend(nullptr);
                },
                "isr_sem_waiter", 4096, &waiter_data, tskIDLE_PRIORITY + 2, &waiter, probe.core);
            contract_assert(created == pdPASS);
            wait_until_blocked(waiter);
        } else if (probe.scenario == isr_scenario::full) {
            probe.initial_release = semaphore.release();
        }

        struct alarm_state {
            binary_semaphore* semaphore;
            isr_release_result result{};
            std::atomic_bool fired = false;
        } alarm_data{&semaphore};
        gptimer_handle_t timer = nullptr;
        const gptimer_config_t config{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                      .direction = GPTIMER_COUNT_UP,
                                      .resolution_hz = 1'000'000,
                                      .intr_priority = 0,
                                      .flags = {}};
        check_driver(gptimer_new_timer(&config, &timer));
        const gptimer_event_callbacks_t callbacks{.on_alarm = [](gptimer_handle_t timer,
                                                                 const gptimer_alarm_event_data_t*,
                                                                 void* argument) noexcept {
            auto& state = *static_cast<alarm_state*>(argument);
            check_driver(gptimer_stop(timer));
            state.result = state.semaphore->release_from_isr();
            state.fired.store(true, std::memory_order_release);
            // Yielding belongs to this caller (via GPTimer), not to the wrapper.
            return state.result.task_woken;
        }};
        check_driver(gptimer_register_event_callbacks(timer, &callbacks, &alarm_data));
        check_driver(gptimer_enable(timer));
        const gptimer_alarm_config_t alarm{.alarm_count = 1000, .reload_count = 0, .flags = {}};
        check_driver(gptimer_set_alarm_action(timer, &alarm));
        // The producer, waiter and interrupt share one core. Start just after a
        // tick so a missing ISR yield is not hidden by the next ordinary tick.
        vTaskDelay(TickType_t{1});
        const auto started = xTaskGetTickCount();
        check_driver(gptimer_start(timer));
        while (!alarm_data.fired.load(std::memory_order_acquire)) {
            if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(1000)) {
                std::abort();
            }
        }
        probe.worker_ran_before_resume = waiter_data.acquired.load();
        probe.result = alarm_data.result;
        if (waiter != nullptr) {
            wait_for_task();
            probe.waiter_acquired = waiter_data.acquired.load();
            vTaskDelete(waiter);
        }
        check_driver(gptimer_disable(timer));
        check_driver(gptimer_del_timer(timer));
        probe.token_remaining = semaphore.try_acquire();
    }
    xTaskNotifyGive(caller);
    vTaskSuspend(nullptr);
}

auto release_in_isr(isr_scenario scenario) -> isr_probe {
    clear_notifications();
    isr_probe probe{
        .scenario = scenario, .caller = xTaskGetCurrentTaskHandle(), .core = xPortGetCoreID()};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreatePinnedToCore(run_isr_probe, "isr_sem_producer", 4096, &probe,
                                                 tskIDLE_PRIORITY + 1, &producer, probe.core);
    REQUIRE(created == pdPASS);
    wait_for_task();
    vTaskDelete(producer);
    return probe;
}

} // namespace

TEST_CASE("FreeRTOS ISR semaphore release reports success and leaves yielding to its caller",
          "[freertos][semaphore][isr]") {
    SECTION("an empty semaphore accepts a token without waking a task") {
        const auto probe = release_in_isr(isr_scenario::empty);

        REQUIRE(probe.result.released);
        REQUIRE_FALSE(probe.result.task_woken);
        REQUIRE(probe.token_remaining);
    }

    SECTION("a full semaphore rejects another token without waking a task") {
        const auto probe = release_in_isr(isr_scenario::full);

        REQUIRE(probe.initial_release);
        REQUIRE_FALSE(probe.result.released);
        REQUIRE_FALSE(probe.result.task_woken);
        REQUIRE(probe.token_remaining);
    }

    SECTION("a higher-priority waiter consumes the token after the caller yields") {
        const auto probe = release_in_isr(isr_scenario::waiting_task);

        REQUIRE(probe.result.released);
        REQUIRE(probe.result.task_woken);
        REQUIRE(probe.waiter_acquired);
        REQUIRE(probe.worker_ran_before_resume);
        REQUIRE_FALSE(probe.token_remaining);
    }
}
#endif
