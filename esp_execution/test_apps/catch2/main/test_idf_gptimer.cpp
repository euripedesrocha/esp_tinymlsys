// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution.hpp"
#include "esp_execution/idf/gptimer.hpp"
#include "esp_freertos_cpp_test/support.hpp"

namespace {
namespace execution = esp::execution;
using esp::freertos::binary_semaphore;
using esp_freertos_cpp_test::wait_for_completion;
using execution::idf::gptimer;
using namespace std::chrono_literals;
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
