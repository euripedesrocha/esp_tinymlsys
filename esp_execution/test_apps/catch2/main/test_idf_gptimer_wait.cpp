// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <tuple>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <esp_timer.h>

#include "esp_execution.hpp"
#include "esp_freertos_cpp_test/support.hpp"
#include "esp_execution/freertos/wait.hpp"
#include "esp_execution/idf/gptimer_context.hpp"
#include "esp_execution_test/freertos.hpp"

namespace {
namespace execution = esp::execution;
using esp::freertos::binary_semaphore;
using esp_execution_test::wait_support::deferred_sender;
using esp_execution_test::wait_support::pending_completion;
using esp_execution_test::wait_support::wait_until_blocked;
using esp_freertos_cpp_test::join_suspended_task;
using esp_freertos_cpp_test::release;
using esp_freertos_cpp_test::wait_for_completion;
using execution::freertos::wait;
using namespace std::chrono_literals;
} // namespace

#if !CONFIG_FREERTOS_UNICORE
TEST_CASE("GPTimer wait can destroy its state while ISR completion returns on the other core",
          "[idf][gptimer][wait][lifetime][multicore]") {
    const bool stopped = GENERATE(false, true);
    CAPTURE(stopped);
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        CAPTURE(iteration);
        const auto result = esp_execution_test::wait_support::complete_on_other_core(
            stopped, [](const auto& complete) {
                esp::platform::gptimer timer;
                binary_semaphore callback_done;
                const auto handler = [&](const gptimer_alarm_event_data_t&) noexcept -> bool {
                    complete();
                    esp_freertos_cpp_test::notify_completion(callback_done);
                    return false;
                };
                timer.set_alarm_handler(handler);
                const gptimer_alarm_config_t alarm{
                    .alarm_count = timer.ticks_for(100us), .reload_count = 0, .flags = {}};
                const auto armed = timer.set_alarm(alarm);
                contract_assert(armed.has_value());
                const auto started = timer.start();
                contract_assert(started.has_value());
                wait_for_completion(callback_done);
                const auto joined = timer.stop_and_join();
                contract_assert(joined.has_value());
                timer.clear_alarm_handler();
            });
        REQUIRE(result.waiter_core == 0);
        REQUIRE(result.completion_core == 1);
        REQUIRE(result.completion_from_isr);
        REQUIRE(result.value_correct);
        REQUIRE(result.returned);
        REQUIRE(result.destroyed);
    }
}
#endif

TEST_CASE("FreeRTOS wait accepts value and stopped completion directly from the timer ISR",
          "[idf][gptimer][freertos][wait][isr]") {
    execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                           .direction = GPTIMER_COUNT_UP,
                                           .resolution_hz = 1'000'000,
                                           .intr_priority = 0,
                                           .flags = {}}};
    bool from_isr = false;
    auto alarm = execution::idf::schedule_after(timer.get_scheduler(), 1ms) |
                 execution::then([&]() noexcept { from_isr = xPortInIsrContext(); });
    SECTION("value") {
        auto work = alarm | execution::then([]() noexcept { return 42; }) |
                    execution::upon_error([](auto) noexcept { return -1; });
        const auto result = wait(work);
        REQUIRE(result.has_value());
        REQUIRE(std::get<0>(*result) == 42);
    }
    SECTION("stopped") {
        auto work = alarm |
                    execution::let_value([]() noexcept { return execution::just_stopped(); }) |
                    execution::upon_error([](auto) noexcept { return -1; });
        REQUIRE_FALSE(wait(work).has_value());
    }
    REQUIRE(from_isr);
}

TEST_CASE("FreeRTOS wait yields from the ISR to the higher-priority waiting task",
          "[idf][gptimer][freertos][wait][isr][yield]") {
    binary_semaphore done;
    struct probe {
        binary_semaphore& done;
        BaseType_t core;
        bool yielded = true;
        bool values_correct = true;
        unsigned observations = 0;
    } result{done, xPortGetCoreID()};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreatePinnedToCore(
        [](void* argument) {
            auto& result = *static_cast<probe*>(argument);
            {
                esp::platform::gptimer timer;
                for (unsigned attempt = 0; attempt < 64 && result.observations < 8; ++attempt) {
                    pending_completion pending;
                    binary_semaphore waiter_done;
                    std::atomic_bool resumed = false;
                    std::atomic_bool alarm_fired = false;
                    struct waiter_state {
                        pending_completion& pending;
                        binary_semaphore& done;
                        std::atomic_bool& resumed;
                        bool valid = false;
                    } state{pending, waiter_done, resumed};
                    TaskHandle_t waiter = nullptr;
                    const auto created = xTaskCreatePinnedToCore(
                        [](void* argument) {
                            auto& state = *static_cast<waiter_state*>(argument);
                            const auto value = wait(deferred_sender{state.pending});
                            state.valid = value && std::get<0>(*value) == 42;
                            state.resumed.store(true);
                            release(state.done);
                            vTaskSuspend(nullptr);
                        },
                        "irq_waiter", 4096, &state, tskIDLE_PRIORITY + 3, &waiter, result.core);
                    if (created != pdPASS)
                        std::abort();
                    wait_for_completion(pending.ready);
                    wait_until_blocked(waiter);
                    const auto handler = [&](const gptimer_alarm_event_data_t&) noexcept -> bool {
                        pending.complete(pending.operation);
                        alarm_fired.store(true);
                        return false; // wait's internal receiver requests the ISR yield.
                    };
                    timer.set_alarm_handler(handler);
                    const auto reset = timer.set_count(0);
                    contract_assert(reset.has_value());
                    const gptimer_alarm_config_t alarm{
                        .alarm_count = timer.ticks_for(100us), .reload_count = 0, .flags = {}};
                    const auto armed = timer.set_alarm(alarm);
                    contract_assert(armed.has_value());
                    // Exclude an ordinary tick from the observation of ISR yielding.
                    vTaskDelay(TickType_t{1});
                    const auto tick = xTaskGetTickCount();
                    const auto started_at = esp_timer_get_time();
                    const auto started = timer.start();
                    contract_assert(started.has_value());
                    while (!alarm_fired.load()) {
                        if (esp_timer_get_time() - started_at >= 1'000'000)
                            std::abort();
                    }
                    const auto ran_before_resume = resumed.load();
                    if (xTaskGetTickCount() == tick) {
                        ++result.observations;
                        result.yielded &= ran_before_resume;
                    }
                    const auto joined = timer.stop_and_join();
                    contract_assert(joined.has_value());
                    timer.clear_alarm_handler();
                    wait_for_completion(waiter_done);
                    join_suspended_task(waiter);
                    result.values_correct &= state.valid && pending.destroyed.load();
                }
            }
            release(result.done);
            vTaskSuspend(nullptr);
        },
        "irq_wait_producer", 6144, &result, tskIDLE_PRIORITY + 1, &producer, result.core);
    REQUIRE(created == pdPASS);
    wait_for_completion(done);
    join_suspended_task(producer);
    REQUIRE(result.observations == 8);
    REQUIRE(result.yielded);
    REQUIRE(result.values_correct);
}
