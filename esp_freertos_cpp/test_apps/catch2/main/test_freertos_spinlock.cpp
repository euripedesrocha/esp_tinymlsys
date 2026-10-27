// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_freertos_cpp/critical_section.hpp"

#include <atomic>
#include <cstdlib>
#include <mutex>

#include <catch2/catch_test_macros.hpp>

#if defined(portTRY_ENTER_CRITICAL_SAFE) && !CONFIG_FREERTOS_UNICORE
#include <esp_timer.h>
#endif

#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_GPTIMER_ISR_CACHE_SAFE
#include <driver/gptimer.h>
#endif

#include "esp_freertos_cpp_test/support.hpp"

namespace {
using esp::freertos::binary_semaphore;
using esp::freertos::spinlock;
using esp_freertos_cpp_test::join_suspended_task;
using esp_freertos_cpp_test::release;
using esp_freertos_cpp_test::wait_for_completion;

#if defined(portTRY_ENTER_CRITICAL_SAFE) && !CONFIG_FREERTOS_UNICORE
void wait_for_flag(const std::atomic_bool& flag) {
    const auto started = esp_timer_get_time();
    while (!flag.load()) {
        // Neither task may unwind storage while the other still borrows it.
        if (esp_timer_get_time() - started >= 100'000)
            std::abort();
    }
}
#endif
} // namespace

TEST_CASE("FreeRTOS spinlock supports basic standard guards", "[freertos][spinlock]") {
    spinlock mutex;
    int value = 0;
#if !CONFIG_IDF_TARGET_LINUX
    const bool initially_enabled = xPortCanYield();
    bool masked = true;
#endif
    {
        std::lock_guard guard{mutex};
        ++value;
#if !CONFIG_IDF_TARGET_LINUX
        masked = masked && !xPortCanYield();
#endif
    }
    {
        std::unique_lock guard{mutex};
        ++value;
#if !CONFIG_IDF_TARGET_LINUX
        masked = masked && !xPortCanYield();
#endif
    }
    {
        std::scoped_lock guard{mutex};
        ++value;
#if !CONFIG_IDF_TARGET_LINUX
        masked = masked && !xPortCanYield();
#endif
    }
    REQUIRE(value == 3);
#if !CONFIG_IDF_TARGET_LINUX
    REQUIRE(initially_enabled);
    REQUIRE(masked);
    REQUIRE(xPortCanYield());
#endif
}

#if defined(portTRY_ENTER_CRITICAL_SAFE) && !CONFIG_FREERTOS_UNICORE
TEST_CASE("FreeRTOS spinlock works with standard guards under cross-core contention",
          "[freertos][spinlock][multicore]") {
    struct fixture {
        spinlock mutex;
        binary_semaphore holder_start;
        binary_semaphore contender_start;
        binary_semaphore holder_done;
        binary_semaphore contender_done;
        std::atomic_bool held = false;
        std::atomic_bool release_holder = false;
        std::atomic_bool unlocked = false;
        bool rejected = false;
        bool enabled_before_rejection = false;
        bool enabled_after_rejection = false;
        bool acquired = false;
        int value = 0;
    } state;

    TaskHandle_t holder = nullptr;
    const auto holder_created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& state = *static_cast<fixture*>(argument);
            wait_for_completion(state.holder_start);
            {
                std::lock_guard guard{state.mutex};
                state.value = 1;
                state.held.store(true);
                wait_for_flag(state.release_holder);
            }
            state.unlocked.store(true);
            release(state.holder_done);
            vTaskSuspend(nullptr);
        },
        "spinlock_holder", 4096, &state, tskIDLE_PRIORITY + 2, &holder, 0);
    if (holder_created != pdPASS)
        std::abort();

    TaskHandle_t contender = nullptr;
    const auto contender_created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& state = *static_cast<fixture*>(argument);
            wait_for_completion(state.contender_start);
            wait_for_flag(state.held);
            {
                state.enabled_before_rejection = xPortCanYield();
                std::unique_lock guard{state.mutex, std::try_to_lock};
                state.enabled_after_rejection = xPortCanYield();
                state.rejected = !guard.owns_lock();
            }
            state.release_holder.store(true);
            wait_for_flag(state.unlocked);
            {
                std::unique_lock guard{state.mutex, std::try_to_lock};
                state.acquired = guard.owns_lock();
                if (state.acquired)
                    ++state.value;
            }
            {
                spinlock other;
                std::scoped_lock guard{state.mutex, other};
                ++state.value;
            }
            release(state.contender_done);
            vTaskSuspend(nullptr);
        },
        "spinlock_contender", 4096, &state, tskIDLE_PRIORITY + 2, &contender, 1);
    if (contender_created != pdPASS)
        std::abort();

    release(state.contender_start);
    release(state.holder_start);
    wait_for_completion(state.holder_done);
    wait_for_completion(state.contender_done);
    join_suspended_task(holder);
    join_suspended_task(contender);

    REQUIRE(state.rejected);
    REQUIRE(state.enabled_before_rejection);
    REQUIRE(state.enabled_after_rejection);
    REQUIRE(state.acquired);
    REQUIRE(state.value == 3);
}

#endif

// The native callback is not placed in IRAM, so exclude cache-safe GPTimer builds.
#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_GPTIMER_ISR_CACHE_SAFE
TEST_CASE("FreeRTOS spinlock can be acquired in an ISR and reacquired by its task",
          "[freertos][spinlock][isr]") {
    struct fixture {
        spinlock mutex;
        binary_semaphore fired;
        binary_semaphore done;
        int value = 1;
        int observed_in_task = 0;
        bool in_isr = false;
        bool task_interrupts_restored = false;
    } state;

    TaskHandle_t worker = nullptr;
    const auto created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& state = *static_cast<fixture*>(argument);
            const auto check_driver = [](esp_err_t result) noexcept {
                if (result != ESP_OK)
                    std::abort(); // An active callback may still borrow the fixture.
            };
            gptimer_handle_t timer = nullptr;
            const gptimer_config_t config{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                          .direction = GPTIMER_COUNT_UP,
                                          .resolution_hz = 1'000'000,
                                          .intr_priority = 0,
                                          .flags = {}};
            check_driver(gptimer_new_timer(&config, &timer));
            const gptimer_event_callbacks_t callbacks{
                .on_alarm = [](gptimer_handle_t, const gptimer_alarm_event_data_t*,
                               void* argument) noexcept {
                    auto& state = *static_cast<fixture*>(argument);
                    {
                        std::lock_guard guard{state.mutex};
                        state.in_isr = xPortInIsrContext() != 0;
                        ++state.value;
                    }
                    const auto result = state.fired.release_from_isr();
                    if (!result.released)
                        std::abort();
                    return result.task_woken;
                }};
            check_driver(gptimer_register_event_callbacks(timer, &callbacks, &state));
            check_driver(gptimer_enable(timer));
            const gptimer_alarm_config_t alarm{.alarm_count = 1000, .reload_count = 0, .flags = {}};
            check_driver(gptimer_set_alarm_action(timer, &alarm));
            check_driver(gptimer_start(timer));
            wait_for_completion(state.fired);
            // Allocation and this pinned worker share a core: the callback has
            // returned before this task can resume and release its storage.
            check_driver(gptimer_stop(timer));
            check_driver(gptimer_disable(timer));
            check_driver(gptimer_del_timer(timer));
            {
                std::lock_guard guard{state.mutex};
                state.observed_in_task = state.value;
                ++state.value;
            }
            state.task_interrupts_restored = xPortCanYield();
            release(state.done);
            vTaskSuspend(nullptr);
        },
        "spinlock_isr", 4096, &state, tskIDLE_PRIORITY + 2, &worker, xPortGetCoreID());
    REQUIRE(created == pdPASS); // No worker borrows the fixture on failure.
    wait_for_completion(state.done);
    join_suspended_task(worker);

    REQUIRE(state.in_isr);
    REQUIRE(state.observed_in_task == 2);
    REQUIRE(state.value == 3);
    REQUIRE(state.task_interrupts_restored);
}
#endif
