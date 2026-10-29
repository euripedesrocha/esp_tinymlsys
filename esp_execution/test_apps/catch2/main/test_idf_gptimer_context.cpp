// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <limits>
#include <new>

#include <catch2/catch_test_macros.hpp>
#include <esp_timer.h>

#include "esp_execution.hpp"
#include "esp_execution/idf/gptimer_context.hpp"
#include "esp_freertos_cpp_test/support.hpp"

namespace {
namespace execution = esp::execution;
using esp::freertos::binary_semaphore;
using esp_freertos_cpp_test::notify_completion;
using esp_freertos_cpp_test::wait_for_completion;
using execution::idf::gptimer;
using execution::idf::gptimer_context;
using execution::idf::gptimer_schedule_error;
using execution::idf::schedule_after;
using namespace std::chrono_literals;

struct completion {
    binary_semaphore& done;
    std::atomic_int values = 0;
    std::atomic_int errors = 0;
    std::atomic_bool from_isr = false;
    std::atomic_bool start_returned = false;
    std::atomic_bool returned_before_completion = false;
    std::atomic<std::int64_t> completed_at = 0;
};

struct receiver {
    using receiver_concept = execution::receiver_tag;
    completion& result;

    void set_value() && noexcept {
        result.completed_at.store(esp_timer_get_time());
        result.from_isr.store(xPortInIsrContext());
        result.returned_before_completion.store(result.start_returned.load());
        result.values.fetch_add(1);
        notify_completion(result.done);
    }

    void set_error(gptimer_schedule_error) && noexcept {
        result.errors.fetch_add(1);
        notify_completion(result.done);
    }

    void set_error(esp_err_t) && noexcept {
        result.from_isr.store(xPortInIsrContext());
        result.errors.fetch_add(1);
        notify_completion(result.done);
    }
};
} // namespace

TEST_CASE("GPTimer scheduling is lazy, starts before its alarm, and completes once in the ISR",
          "[idf][gptimer][scheduler]") {
    constexpr auto delay = 20ms;
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context context{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                             .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 1'000'000,
                             .intr_priority = 0,
                             .flags = {}}};
    const auto scheduler = context.get_scheduler();
    STATIC_REQUIRE(execution::scheduler<decltype(scheduler)>);
    auto work = schedule_after(scheduler, delay);
    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(execution::get_env(work)) ==
            scheduler);
    auto operation = execution::connect(work, receiver{result});
    vTaskDelay(TickType_t{2});
    REQUIRE(result.values.load() == 0);
    REQUIRE(result.errors.load() == 0);
    const auto started = esp_timer_get_time();
    execution::start(operation);
    result.start_returned.store(true);
    wait_for_completion(done);
    REQUIRE(result.returned_before_completion.load());
    REQUIRE(result.completed_at.load() - started >=
            std::chrono::duration_cast<std::chrono::microseconds>(delay).count());
    REQUIRE(result.values.load() == 1);
    REQUIRE(result.errors.load() == 0);
    REQUIRE(result.from_isr.load());
    REQUIRE_FALSE(done.try_acquire_for(30ms));
    REQUIRE(result.values.load() == 1);
    REQUIRE(result.errors.load() == 0);
}

TEST_CASE("GPTimer immediate and successive schedules use the same context",
          "[idf][gptimer][scheduler]") {
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context context{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                             .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 1'000'000,
                             .intr_priority = 0,
                             .flags = {}}};
    const auto scheduler = context.get_scheduler();
    auto first = execution::connect(execution::schedule(scheduler), receiver{result});
    execution::start(first);
    wait_for_completion(done);
    auto second = execution::connect(schedule_after(scheduler, -1ms), receiver{result});
    execution::start(second);
    wait_for_completion(done);
    auto third = execution::connect(schedule_after(scheduler, 1ns), receiver{result});
    execution::start(third);
    wait_for_completion(done);
    REQUIRE(result.values.load() == 3);
    REQUIRE(result.errors.load() == 0);
    REQUIRE(result.from_isr.load());
}

TEST_CASE("GPTimer relative deadlines account for fractional counter ticks",
          "[idf][gptimer][scheduler]") {
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context context{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                             .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 2000,
                             .intr_priority = 0,
                             .flags = {}}};
    auto shortest = std::numeric_limits<std::int64_t>::max();
    for (int iteration = 0; iteration < 16; ++iteration) {
        auto operation =
            execution::connect(schedule_after(context.get_scheduler(), 500us), receiver{result});
        const auto started = esp_timer_get_time();
        execution::start(operation);
        wait_for_completion(done);
        shortest = std::min(shortest, result.completed_at.load() - started);
    }
    REQUIRE(shortest >= 500);
    REQUIRE(result.values.load() == 16);
    REQUIRE(result.errors.load() == 0);
}

TEST_CASE("GPTimer busy rejection preserves the accepted deadline", "[idf][gptimer][scheduler]") {
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context context{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                             .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 1'000'000,
                             .intr_priority = 0,
                             .flags = {}}};
    auto accepted =
        execution::connect(schedule_after(context.get_scheduler(), 20ms), receiver{result});
    struct rejection_receiver {
        using receiver_concept = execution::receiver_tag;
        bool& rejected;
        void set_value() && noexcept { std::abort(); }
        void set_error(esp_err_t) && noexcept { std::abort(); }
        void set_error(gptimer_schedule_error error) && noexcept {
            rejected = error == gptimer_schedule_error::busy && !xPortInIsrContext();
        }
    };
    bool rejected = false;
    auto second = execution::connect(execution::schedule(context.get_scheduler()),
                                     rejection_receiver{rejected});
    execution::start(accepted);
    execution::start(second);
    const auto inline_rejection = rejected;
    wait_for_completion(done);
    REQUIRE(inline_rejection);
    REQUIRE(result.values.load() == 1);
    REQUIRE(result.errors.load() == 0);
}

TEST_CASE("GPTimer rejects an unrepresentable deadline and remains reusable",
          "[idf][gptimer][scheduler]") {
    // A 64-bit counter covers every nanosecond duration at this resolution.
    if (gptimer::max_count() == std::numeric_limits<std::uint64_t>::max())
        return;
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context context{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                             .direction = GPTIMER_COUNT_UP,
                             .resolution_hz = 2'000'000,
                             .intr_priority = 0,
                             .flags = {}}};
    struct range_receiver {
        using receiver_concept = execution::receiver_tag;
        bool& rejected;
        void set_value() && noexcept { std::abort(); }
        void set_error(esp_err_t) && noexcept { std::abort(); }
        void set_error(gptimer_schedule_error error) && noexcept {
            rejected = error == gptimer_schedule_error::deadline_out_of_range;
        }
    };
    bool rejected = false;
    auto invalid =
        execution::connect(schedule_after(context.get_scheduler(), std::chrono::nanoseconds::max()),
                           range_receiver{rejected});
    execution::start(invalid);
    REQUIRE(rejected);
    auto valid = execution::connect(execution::schedule(context.get_scheduler()), receiver{result});
    execution::start(valid);
    wait_for_completion(done);
    REQUIRE(result.values.load() == 1);
    REQUIRE(result.errors.load() == 0);
}

TEST_CASE("GPTimer completion can destroy its operation and immediately schedule another deadline",
          "[idf][gptimer][scheduler][lifetime]") {
    binary_semaphore done;
    completion result{.done = done};
    gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                           .direction = GPTIMER_COUNT_UP,
                           .resolution_hz = 1'000'000,
                           .intr_priority = 0,
                           .flags = {}}};
    auto next = execution::connect(schedule_after(timer.get_scheduler(), 1ms), receiver{result});
    struct destruction_record {
        void* operation = nullptr;
        void (*destroy)(void*) noexcept = nullptr;
        bool destroyed = false;
    } record;
    struct destroying_receiver {
        using receiver_concept = execution::receiver_tag;
        destruction_record& record;
        decltype(next)& following;

        void set_value() && noexcept {
            auto& state = record;
            auto& next = following;
            state.destroy(std::exchange(state.operation, nullptr));
            state.destroyed = true;
            execution::start(next);
        }
        void set_error(esp_err_t) && noexcept { std::abort(); }
        void set_error(gptimer_schedule_error) && noexcept { std::abort(); }
    };
    using operation_type = decltype(execution::connect(schedule_after(timer.get_scheduler(), 1ms),
                                                       destroying_receiver{record, next}));
    alignas(operation_type) std::byte storage[sizeof(operation_type)];
    auto* first = ::new (static_cast<void*>(storage)) operation_type(execution::connect(
        schedule_after(timer.get_scheduler(), 1ms), destroying_receiver{record, next}));
    record.operation = first;
    record.destroy = [](void* state) noexcept {
        std::destroy_at(static_cast<operation_type*>(state));
    };

    execution::start(*first);
    wait_for_completion(done);
    REQUIRE(record.destroyed);
    REQUIRE(record.operation == nullptr);
    REQUIRE(result.values.load() == 1);
    REQUIRE(result.errors.load() == 0);
    REQUIRE(result.from_isr.load());
}
