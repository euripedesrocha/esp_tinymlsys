// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>

#include <catch2/catch_test_macros.hpp>

#include "esp_freertos_cpp/event_group.hpp"
#include "esp_freertos_cpp/semaphore.hpp"

namespace {

using esp::freertos::binary_semaphore;
using esp::freertos::clear_on_exit;
using esp::freertos::event_group;
using esp::freertos::tick_duration;
using namespace std::chrono_literals;

constexpr EventBits_t first_bit = EventBits_t{1} << 0;
constexpr EventBits_t second_bit = EventBits_t{1} << 1;
constexpr EventBits_t other_bit = EventBits_t{1} << 2;

[[noreturn]] void task_failure(const char* message) {
    // The task may still borrow test storage. Do not unwind that storage.
    std::fputs(message, stderr);
    std::abort();
}

void wait_for_state(TaskHandle_t task, eTaskState expected) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != expected) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000))
            task_failure("Event-group test task did not reach the expected state\n");
        vTaskDelay(TickType_t{1});
    }
}

} // namespace

TEST_CASE("event group forwards masks and native snapshots", "[freertos][event_group]") {
    event_group events;
    REQUIRE(events.get_bits() == 0);
    REQUIRE(events.set(first_bit | other_bit) == (first_bit | other_bit));
    REQUIRE(events.get_bits() == (first_bit | other_bit));
    REQUIRE(events.clear(first_bit) == (first_bit | other_bit));
    REQUIRE(events.get_bits() == other_bit);
}

TEST_CASE("event group maps wait conditions and clearing to optional snapshots",
          "[freertos][event_group]") {
    event_group events;
    events.set(first_bit | other_bit);

    SECTION("any returns the full snapshot when one requested bit matches") {
        const auto result =
            events.wait_any_for(first_bit | second_bit, 0ns, clear_on_exit::preserve);
        REQUIRE(result.has_value());
        REQUIRE(*result == (first_bit | other_bit));
        REQUIRE(events.get_bits() == (first_bit | other_bit));
    }

    SECTION("all returns no value for a partial match and leaves the bits intact") {
        const auto result = events.wait_all_for(first_bit | second_bit, 0ns, clear_on_exit::clear);
        REQUIRE_FALSE(result.has_value());
        REQUIRE(events.get_bits() == (first_bit | other_bit));
    }

    SECTION("all returns the pre-clear snapshot and clears only the requested bits") {
        events.set(second_bit);
        const auto result = events.wait_all(first_bit | second_bit, clear_on_exit::clear);
        REQUIRE(result.has_value());
        REQUIRE(*result == (first_bit | second_bit | other_bit));
        REQUIRE(events.get_bits() == other_bit);
    }
}

TEST_CASE("event group forwards a finite chrono timeout", "[freertos][event_group][timeout]") {
    constexpr TickType_t wait_ticks = 2;
    constexpr auto timeout =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tick_duration{wait_ticks});
    event_group events;
    events.set(other_bit);

    const auto started = xTaskGetTickCount();
    const auto result = events.wait_any_for(first_bit, timeout, clear_on_exit::preserve);
    const auto elapsed = xTaskGetTickCount() - started;

    REQUIRE(elapsed >= wait_ticks);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(events.get_bits() == other_bit);
}

TEST_CASE("event group forwards a blocking any wait and returns its snapshot",
          "[freertos][event_group][tasks]") {
    event_group events;
    events.set(other_bit);
    struct waiter_state {
        event_group& events;
        std::optional<EventBits_t> result{};
        binary_semaphore completed{};
    } state{.events = events};

    TaskHandle_t waiter = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<waiter_state*>(argument);
            state.result = state.events.wait_any(first_bit | second_bit, clear_on_exit::clear);
            const bool released = state.completed.release();
            contract_assert(released);
            vTaskSuspend(nullptr);
        },
        "event_waiter", 4096, &state, tskIDLE_PRIORITY + 1, &waiter);
    REQUIRE(created == pdPASS); // No task borrows test storage on failure.

    wait_for_state(waiter, eBlocked);
    const auto after = events.set(first_bit);
    if (!state.completed.try_acquire_for(5s))
        task_failure("Event-group test task timed out\n");
    // Wait for release() to finish before destroying its semaphore.
    wait_for_state(waiter, eSuspended);
    vTaskDelete(waiter);

    REQUIRE(after == other_bit);
    REQUIRE(state.result.has_value());
    REQUIRE(*state.result == (first_bit | other_bit));
    REQUIRE(events.get_bits() == other_bit);
}
