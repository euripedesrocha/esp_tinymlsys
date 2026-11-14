// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>

#include <catch2/catch_test_macros.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_freertos_cpp/semaphore.hpp"
#include "esp_platform_cpp/gptimer.hpp"

namespace {
using esp::freertos::binary_semaphore;
using esp::platform::gptimer;
using namespace std::chrono_literals;

// A timeout must not unwind state that a task or ISR may still be using.
void wait_for_completion(binary_semaphore& semaphore) {
    if (!semaphore.try_acquire_for(5s)) {
        std::fputs("Timed out waiting for completion; cannot safely unwind\n", stderr);
        std::abort();
    }
}
} // namespace

TEST_CASE("GPTimer counter access and chrono conversion follow its resolution", "[idf][gptimer]") {
    std::uint32_t rate = 1'000'000;
    SECTION("one microsecond ticks") { rate = 1'000'000; }
    SECTION("half microsecond ticks") { rate = 2'000'000; }
    gptimer timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                   .direction = GPTIMER_COUNT_UP,
                   .resolution_hz = rate,
                   .intr_priority = 0,
                   .flags = {}}};

    REQUIRE(timer.resolution_hz() == rate);
    const auto tick = std::chrono::nanoseconds{1'000'000'000 / rate};
    REQUIRE(timer.ticks_for(0ns) == 0);
    REQUIRE(timer.ticks_for(1ns) == 1);
    REQUIRE(timer.ticks_for(tick) == 1);
    REQUIRE(timer.ticks_for(tick + 1ns) == 2);
    REQUIRE(timer.ticks_for(1ms) == rate / 1000);

    REQUIRE(timer.set_count(timer.max_count()).has_value());
    const auto maximum = timer.count();
    REQUIRE(maximum.has_value());
    REQUIRE(*maximum == timer.max_count());
    REQUIRE(timer.set_count(0).has_value());
    const auto reset = timer.count();
    REQUIRE(reset.has_value());
    REQUIRE(*reset == 0);
}

TEST_CASE("GPTimer preserves the native counting direction", "[idf][gptimer]") {
    gptimer timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                   .direction = GPTIMER_COUNT_DOWN,
                   .resolution_hz = 1'000'000,
                   .intr_priority = 0,
                   .flags = {}}};
    const auto initial = gptimer::max_count() / 2;
    REQUIRE(timer.set_count(initial).has_value());
    REQUIRE(timer.start().has_value());
    vTaskDelay(TickType_t{1});
    REQUIRE(timer.stop_and_join().has_value());
    const auto current = timer.count();
    REQUIRE(current.has_value());
    REQUIRE(*current < initial);
}

TEST_CASE("GPTimer preserves driver alarm configuration errors", "[idf][gptimer][error]") {
    gptimer timer;
    const gptimer_alarm_config_t invalid{
        .alarm_count = 10,
        .reload_count = 10,
        .flags = {.auto_reload_on_alarm = true},
    };
    const auto rejected = timer.set_alarm(invalid);
    REQUIRE_FALSE(rejected.has_value());
    REQUIRE(rejected.error() == ESP_ERR_INVALID_ARG);
    REQUIRE(timer.clear_alarm().has_value());
}

TEST_CASE("GPTimer joins a periodic handler and can reuse the counter", "[idf][gptimer][handler]") {
    binary_semaphore done;
    std::atomic_uint calls = 0;
    std::atomic_bool correct_event = true;
    // Deliberately const: binding borrows the callable with its original cv qualification.
    const auto handler = [&](const gptimer_alarm_event_data_t& event) noexcept -> bool {
        if (!xPortInIsrContext() || event.alarm_value != 1000)
            correct_event.store(false);
        if (calls.fetch_add(1) == 0) {
            const auto released = done.release_from_isr();
            contract_assert(released.released);
            return released.task_woken;
        }
        return false;
    };
    gptimer timer;
    timer.set_alarm_handler(handler);
    const gptimer_alarm_config_t periodic{
        .alarm_count = 1000, .reload_count = 0, .flags = {.auto_reload_on_alarm = true}};
    REQUIRE(timer.set_alarm(periodic).has_value());
    REQUIRE(timer.start().has_value());
    wait_for_completion(done);
    REQUIRE(timer.stop_and_join().has_value());
    const auto calls_at_stop = calls.load();
    vTaskDelay(TickType_t{2});
    REQUIRE(calls.load() == calls_at_stop);
    REQUIRE(calls_at_stop >= 1);
    REQUIRE(correct_event.load());

    timer.clear_alarm_handler();
    REQUIRE(timer.clear_alarm().has_value());
    REQUIRE(timer.set_count(0).has_value());
    REQUIRE(timer.start().has_value());
    vTaskDelay(TickType_t{1});
    REQUIRE(timer.stop().has_value());
    REQUIRE(timer.stop_and_join().has_value());
    const auto count = timer.count();
    REQUIRE(count.has_value());
    REQUIRE(*count > 0);
    REQUIRE(calls.load() == calls_at_stop);
}

// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <sdkconfig.h>

#if !CONFIG_FREERTOS_UNICORE

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <optional>

#include <catch2/catch_test_macros.hpp>
#include <esp_ipc.h>
#include <esp_timer.h>

#include "esp_platform_cpp/gptimer.hpp"

namespace {
using esp::freertos::binary_semaphore;
using esp::platform::gptimer;
using namespace std::chrono_literals;

void release(binary_semaphore& semaphore) noexcept {
    const bool released = semaphore.release();
    contract_assert(released);
}

// Completion can be signaled before its producer stops using borrowed state.
void join_suspended_task(TaskHandle_t task) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != eSuspended) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000))
            std::abort();
        vTaskDelay(TickType_t{1});
    }
    vTaskDelete(task);
}

constexpr BaseType_t controller_core = 0;
constexpr BaseType_t interrupt_core = 1;
constexpr std::int64_t gate_timeout_us = 100'000;

enum class callback_phase { executing, pending };

// Bounded spins are necessary while the other core's interrupts are held.
// A timeout cannot safely unwind a live ISR or its borrowed test state.
void wait_for_flag(const std::atomic_bool& flag) {
    const auto started = esp_timer_get_time();
    while (!flag.load()) {
        if (esp_timer_get_time() - started >= gate_timeout_us)
            std::abort();
    }
}

void require_success(std::expected<void, esp_err_t> result) {
    if (!result)
        std::abort();
}

struct observation {
    bool scenario_ready = false;
    bool join_waited = false;
    bool join_succeeded = false;
    bool callback_finished_at_return = false;
    bool callback_was_remote_isr = false;
    unsigned original_calls = 0;
    unsigned replacement_calls = 0;
};

struct fixture {
    callback_phase phase;
    binary_semaphore ready{};
    binary_semaphore mask_request{};
    binary_semaphore join_request{};
    binary_semaphore joined{};
    std::atomic_bool interrupts_masked = false;
    std::atomic_bool callback_entered = false;
    std::atomic_bool callback_finished = false;
    std::atomic_bool gate_open = false;
    std::atomic_bool join_called = false;
    std::atomic_bool join_returned = false;
    std::atomic_uint calls = 0;
    bool remote_isr = false;
    bool join_succeeded = false;
    bool finished_at_return = false;
    portMUX_TYPE interrupt_mask = portMUX_INITIALIZER_UNLOCKED;
    std::optional<gptimer> timer{};

    bool operator()(const gptimer_alarm_event_data_t&) noexcept {
        remote_isr = xPortInIsrContext() && xPortGetCoreID() == interrupt_core;
        calls.fetch_add(1);
        callback_entered.store(true);
        if (phase == callback_phase::executing)
            wait_for_flag(gate_open);
        callback_finished.store(true);
        return false;
    }
};

void initialize_on_interrupt_core(void* argument) {
    auto& state = *static_cast<fixture*>(argument);
    state.timer.emplace(gptimer_config_t{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                         .direction = GPTIMER_COUNT_UP,
                                         .resolution_hz = 1'000'000,
                                         .intr_priority = 1,
                                         .flags = {}});
    state.timer->set_alarm_handler(state);
    release(state.ready);

    if (state.phase == callback_phase::pending) {
        wait_for_completion(state.mask_request);
        portENTER_CRITICAL(&state.interrupt_mask);
        state.interrupts_masked.store(true);
        wait_for_flag(state.gate_open);
        portEXIT_CRITICAL(&state.interrupt_mask);
    }
    vTaskSuspend(nullptr);
}

void join_on_controller_core(void* argument) {
    auto& state = *static_cast<fixture*>(argument);
    wait_for_completion(state.join_request);
    state.join_called.store(true);
    state.join_succeeded = state.timer->stop_and_join().has_value();
    state.finished_at_return = state.callback_finished.load();
    state.join_returned.store(true);
    release(state.joined);
    vTaskSuspend(nullptr);
}

auto exercise_remote_join(callback_phase phase) -> observation {
    fixture state{.phase = phase};
    TaskHandle_t initializer = nullptr;
    const auto initialized =
        xTaskCreatePinnedToCore(initialize_on_interrupt_core, "timer_irq_setup", 4096, &state,
                                tskIDLE_PRIORITY + 2, &initializer, interrupt_core);
    if (initialized != pdPASS)
        std::abort();
    wait_for_completion(state.ready);

    TaskHandle_t joiner = nullptr;
    const auto created =
        xTaskCreatePinnedToCore(join_on_controller_core, "timer_join", 4096, &state,
                                tskIDLE_PRIORITY + 3, &joiner, controller_core);
    if (created != pdPASS)
        std::abort();

    if (phase == callback_phase::pending) {
        release(state.mask_request);
        wait_for_flag(state.interrupts_masked);
    }
    const auto deadline = state.timer->ticks_for(1ms);
    const gptimer_alarm_config_t alarm{.alarm_count = deadline, .reload_count = 0, .flags = {}};
    require_success(state.timer->set_alarm(alarm));
    require_success(state.timer->start());

    observation result{};
    if (phase == callback_phase::executing) {
        wait_for_flag(state.callback_entered);
        result.scenario_ready = !state.callback_finished.load();
    } else {
        const auto started = esp_timer_get_time();
        while (true) {
            const auto count = state.timer->count();
            if (!count)
                std::abort();
            if (*count >= deadline)
                break;
            if (esp_timer_get_time() - started >= gate_timeout_us)
                std::abort();
        }
        // The counter passed an enabled one-shot alarm while its IRQ was masked.
        result.scenario_ready = state.interrupts_masked.load() && !state.callback_entered.load();
    }

    release(state.join_request);
    wait_for_flag(state.join_called);
    const auto started = esp_timer_get_time();
    while (!state.join_returned.load() && eTaskGetState(joiner) != eBlocked) {
        if (esp_timer_get_time() - started >= gate_timeout_us)
            std::abort();
    }
    result.join_waited = !state.join_returned.load() && eTaskGetState(joiner) == eBlocked;
    state.gate_open.store(true);
    wait_for_completion(state.joined);
    join_suspended_task(joiner);
    join_suspended_task(initializer);

    // Independent cleanup fence: even a broken stop_and_join must not make a
    // failing regression test unwind callback storage still used by the ISR.
    if (esp_ipc_call_blocking(interrupt_core, [](void*) {}, nullptr) != ESP_OK)
        std::abort();
    result.join_succeeded = state.join_succeeded;
    result.callback_finished_at_return = state.finished_at_return;
    result.callback_was_remote_isr = state.remote_isr;
    result.original_calls = state.calls.load();
    require_success(state.timer->stop_and_join());

    binary_semaphore replacement_done;
    std::atomic_uint replacement_calls = 0;
    const auto replacement = [&](const gptimer_alarm_event_data_t&) noexcept -> bool {
        replacement_calls.fetch_add(1);
        const auto released = replacement_done.release_from_isr();
        contract_assert(released.released);
        return released.task_woken;
    };
    state.timer->set_alarm_handler(replacement);
    require_success(state.timer->clear_alarm());
    require_success(state.timer->set_count(0));
    require_success(state.timer->set_alarm(alarm));
    require_success(state.timer->start());
    wait_for_completion(replacement_done);
    require_success(state.timer->stop_and_join());
    state.timer->clear_alarm_handler();
    result.replacement_calls = replacement_calls.load();
    return result;
}

struct controller_state {
    callback_phase phase;
    binary_semaphore& done;
    observation result{};
};

void control_join_scenario(void* argument) {
    auto& state = *static_cast<controller_state*>(argument);
    state.result = exercise_remote_join(state.phase);
    release(state.done);
    vTaskSuspend(nullptr);
}
} // namespace

TEST_CASE("GPTimer joins callbacks already executing or pending on another core",
          "[idf][gptimer][join][multicore]") {
    callback_phase phase = callback_phase::executing;
    SECTION("join waits for an executing callback before its handler can be rebound") {
        phase = callback_phase::executing;
    }
    SECTION("join drains a pending interrupt before its handler can be rebound") {
        phase = callback_phase::pending;
    }

    binary_semaphore done;
    controller_state state{.phase = phase, .done = done};
    TaskHandle_t controller = nullptr;
    const auto created =
        xTaskCreatePinnedToCore(control_join_scenario, "timer_join_control", 8192, &state,
                                tskIDLE_PRIORITY + 2, &controller, controller_core);
    REQUIRE(created == pdPASS);
    wait_for_completion(done);
    join_suspended_task(controller);

    REQUIRE(state.result.scenario_ready);
    REQUIRE(state.result.join_waited);
    REQUIRE(state.result.join_succeeded);
    REQUIRE(state.result.callback_finished_at_return);
    REQUIRE(state.result.callback_was_remote_isr);
    REQUIRE(state.result.original_calls == 1);
    REQUIRE(state.result.replacement_calls == 1);
}

#endif // !CONFIG_FREERTOS_UNICORE
