// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <array>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <esp_timer.h>

#include "esp_execution.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "esp_freertos_cpp_test/support.hpp"
#include "esp_execution/enum_flags.hpp"
#include "esp_freertos_cpp/event_group.hpp"
#include "esp_execution/idf/gptimer_context.hpp"

namespace {
namespace execution = esp::execution;
using esp::execution::freertos::task_context;

using esp::freertos::binary_semaphore;
using esp::freertos::clear_on_exit;
using esp::freertos::event_group;
using esp_freertos_cpp_test::join_suspended_task;
using esp_freertos_cpp_test::notify_completion;
using esp_freertos_cpp_test::release;
using esp_freertos_cpp_test::wait_for_completion;
using namespace std::chrono_literals;

enum class phase : EventBits_t { worker_entered = 1 << 0 };

struct alarm_receiver {
    using receiver_concept = execution::receiver_tag;
    binary_semaphore& done;
    void set_value() && noexcept { notify_completion(done); }
    void set_error(esp_err_t) && noexcept { std::abort(); }
    void set_error(execution::idf::gptimer_schedule_error) && noexcept { std::abort(); }
};

struct result {
    binary_semaphore& done;
    std::atomic_int values = 0;
    std::atomic_int errors = 0;
    std::atomic<execution::freertos::schedule_error> error =
        execution::freertos::schedule_error::queue_timeout;
    std::atomic_int stopped = 0;
    std::atomic_bool from_isr = false;
    std::atomic<TaskHandle_t> task = nullptr;
};

struct receiver {
    using receiver_concept = execution::receiver_tag;
    result& completion;

    void set_value() && noexcept {
        completion.values.fetch_add(1);
        notify();
    }
    void set_error(auto error) && noexcept {
        if constexpr (std::same_as<decltype(error), execution::freertos::schedule_error>)
            completion.error.store(error);
        completion.errors.fetch_add(1);
        notify();
    }
    void set_stopped() && noexcept {
        completion.stopped.fetch_add(1);
        notify();
    }

private:
    void notify() noexcept {
        completion.from_isr.store(xPortInIsrContext());
        if (!xPortInIsrContext())
            completion.task.store(xTaskGetCurrentTaskHandle());
        notify_completion(completion.done);
    }
};

struct frame_pool;

struct frame_lease {
    frame_pool* pool;
    std::size_t slot;
    frame_lease(frame_pool& pool, std::size_t slot) noexcept : pool(&pool), slot(slot) {}
    frame_lease(const frame_lease&) = delete;
    frame_lease(frame_lease&& other) noexcept
        : pool(std::exchange(other.pool, nullptr)), slot(other.slot) {}
    ~frame_lease();
    std::span<const std::int16_t> samples() const noexcept;
};

struct frame_pool {
    std::array<std::array<std::int16_t, 4>, 4> samples{};
    std::array<std::atomic_bool, 4> leased{};
    std::atomic_int acquired = 0;
    std::atomic_int released = 0;
    std::atomic_bool input_from_isr = true;
};

frame_lease::~frame_lease() {
    if (pool) {
        pool->leased[slot].store(false, std::memory_order_release);
        pool->released.fetch_add(1);
    }
}

std::span<const std::int16_t> frame_lease::samples() const noexcept { return pool->samples[slot]; }

auto acquire_frame(frame_pool& pool) noexcept -> frame_lease {
    if (!xPortInIsrContext())
        pool.input_from_isr.store(false);
    for (std::size_t slot = 0; slot < pool.leased.size(); ++slot) {
        if (!pool.leased[slot].exchange(true, std::memory_order_acq_rel)) {
            const int index = pool.acquired.fetch_add(1) + 1;
            pool.samples[slot].fill(static_cast<std::int16_t>(index));
            return frame_lease{pool, slot};
        }
    }
    std::abort(); // This serial test must return each lease before acquiring the next.
}

struct blocked_receiver {
    using receiver_concept = execution::receiver_tag;
    event_group& progress;
    binary_semaphore& worker_release;
    std::atomic_bool* timed_out;
    void set_value() && noexcept {
        progress.set(std::to_underlying(phase::worker_entered));
        if (!worker_release.try_acquire_for(5s)) {
            timed_out->store(true);
        }
    }
    void set_error(auto) && noexcept {}
    void set_stopped() && noexcept {}
};
} // namespace

TEST_CASE("an interrupt transfers a leased frame to dependent task processing",
          "[freertos][isr][pipeline][lifetime]") {
    binary_semaphore done;
    task_context context{
        {.name = "irq_processing", .stack_size = 4096, .priority = 5, .queue_capacity = 4}};
    const auto scheduler = context.get_scheduler();
    result warmup{done};
    auto warmup_operation = execution::connect(execution::schedule(scheduler), receiver{warmup});
    execution::start(warmup_operation);
    wait_for_completion(done);
    const auto worker = warmup.task.load();
    REQUIRE(worker != xTaskGetCurrentTaskHandle());

    execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                           .direction = GPTIMER_COUNT_UP,
                                           .resolution_hz = 1'000'000,
                                           .intr_priority = 0,
                                           .flags = {}}};
    frame_pool pool;
    result completion{done};
    int processed = 0;
    int checksum = 0;
    bool correct_task = true;
    {
        auto work =
            execution::idf::schedule_after(timer.get_scheduler(), 1ms) |
            execution::then([&pool]() noexcept { return acquire_frame(pool); }) |
            execution::continues_on(scheduler) |
            execution::let_value([&](frame_lease& frame) noexcept {
                correct_task &= !xPortInIsrContext() && xTaskGetCurrentTaskHandle() == worker;
                return execution::schedule(scheduler) |
                       execution::then(
                           [&frame, &processed, &checksum, &correct_task, worker]() noexcept {
                               correct_task &=
                                   !xPortInIsrContext() && xTaskGetCurrentTaskHandle() == worker;
                               for (auto sample : frame.samples())
                                   checksum += sample;
                               ++processed;
                           });
            }) |
            execution::repeat_n(64);
        auto operation = execution::connect(work, receiver{completion});
        execution::start(operation);
        wait_for_completion(done);
        REQUIRE(completion.values.load() == 1);
        REQUIRE(completion.errors.load() == 0);
        REQUIRE(completion.stopped.load() == 0);
        REQUIRE(completion.task.load() == worker);
        REQUIRE_FALSE(completion.from_isr.load());
        REQUIRE(processed == 64);
        REQUIRE(checksum == 4 * (64 * 65 / 2));
        REQUIRE(correct_task);
        REQUIRE(pool.input_from_isr.load());
    }
    REQUIRE(pool.acquired.load() == 64);
    REQUIRE(pool.released.load() == 64);
    for (auto& leased : pool.leased)
        REQUIRE_FALSE(leased.load());
}

TEST_CASE("ISR scheduling rejects full and finished contexts inline",
          "[freertos][isr][rejection]") {
    bool finished = false;
    SECTION("a full queue rejects with queue_full") { finished = false; }
    SECTION("a finished context rejects with stopped") { finished = true; }
    event_group progress;
    binary_semaphore worker_release;
    binary_semaphore done;
    binary_semaphore alarm_done;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "irq_rejection", .queue_capacity = 1}};
    const auto scheduler = context->get_scheduler();
    std::atomic_bool timed_out = false;
    result rejected{done};
    auto blocking = execution::connect(execution::schedule(scheduler),
                                       blocked_receiver{progress, worker_release, &timed_out});
    auto queued = execution::connect(execution::schedule(scheduler),
                                     blocked_receiver{progress, worker_release, &timed_out});
    bool worker_entered = true;
    if (finished) {
        context->finish();
    } else {
        execution::start(blocking);
        worker_entered =
            progress
                .wait_all_for(std::to_underlying(phase::worker_entered), 5s, clear_on_exit::clear)
                .has_value();
        if (worker_entered) {
            execution::start(queued);
        }
    }

    auto rejected_operation =
        execution::connect(execution::schedule(scheduler), receiver{rejected});
    bool completed = false;
    bool next_entered = true;
    {
        execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                               .direction = GPTIMER_COUNT_UP,
                                               .resolution_hz = 1'000'000,
                                               .intr_priority = 0,
                                               .flags = {}}};
        auto trigger = execution::idf::schedule_after(timer.get_scheduler(), 1ms) |
                       execution::then([&]() noexcept { execution::start(rejected_operation); });
        auto alarm_operation = execution::connect(std::move(trigger), alarm_receiver{alarm_done});
        if (worker_entered) {
            execution::start(alarm_operation);
            wait_for_completion(done);
            // The inner rejection occurs before then delivers its terminal completion.
            wait_for_completion(alarm_done);
            completed = true;
        }
        // Release accepted callbacks before assertions can unwind the test.
        if (!finished) {
            release(worker_release);
            if (worker_entered) {
                next_entered = progress
                                   .wait_all_for(std::to_underlying(phase::worker_entered), 5s,
                                                 clear_on_exit::clear)
                                   .has_value();
            }
            release(worker_release);
        }
        // Disable the interrupt source before releasing the execution resource.
    }
    context.reset();
    REQUIRE(worker_entered);
    REQUIRE(next_entered);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(completed);
    REQUIRE(rejected.from_isr.load());
    REQUIRE(rejected.values.load() == 0);
    REQUIRE(rejected.errors.load() == (finished ? 0 : 1));
    REQUIRE(rejected.stopped.load() == (finished ? 1 : 0));
    if (!finished)
        REQUIRE(rejected.error.load() == execution::freertos::schedule_error::queue_full);
}

TEST_CASE("ISR scheduling yields to a higher-priority worker on the interrupted core",
          "[freertos][isr][yield]") {
    binary_semaphore done;
    struct yield_probe {
        binary_semaphore& done;
        BaseType_t core;
        bool worker_ran_before_resume = true;
        bool all_completed = true;
        unsigned observations = 0;
    } probe{done, xPortGetCoreID()};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreatePinnedToCore(
        [](void* state) {
            auto& probe = *static_cast<yield_probe*>(state);
            {
                constexpr auto producer_priority = tskIDLE_PRIORITY + 1;
                task_context context{{.name = "irq_yield_worker",
                                      .priority = producer_priority + 1,
                                      .core_id = probe.core,
                                      .queue_capacity = 1}};
                // Allocate the interrupt from this pinned task so it shares the worker's core.
                execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                                       .direction = GPTIMER_COUNT_UP,
                                                       .resolution_hz = 1'000'000,
                                                       .intr_priority = 0,
                                                       .flags = {}}};
                binary_semaphore completed;
                binary_semaphore alarm_done;
                for (int attempt = 0; attempt < 64 && probe.observations < 8; ++attempt) {
                    std::atomic_bool worker_ran = false;
                    std::atomic_bool alarm_fired = false;
                    result completion{completed};
                    auto work =
                        execution::schedule(context.get_scheduler()) |
                        execution::then([&worker_ran]() noexcept { worker_ran.store(true); });
                    auto operation = execution::connect(std::move(work), receiver{completion});
                    auto trigger = execution::idf::schedule_after(timer.get_scheduler(), 100us) |
                                   execution::then([&]() noexcept {
                                       execution::start(operation);
                                       alarm_fired.store(true, std::memory_order_release);
                                   });
                    auto alarm_operation =
                        execution::connect(std::move(trigger), alarm_receiver{alarm_done});
                    // Let the worker block and start just after a tick. The next ordinary
                    // tick must not be what makes an otherwise missing ISR yield look correct.
                    vTaskDelay(TickType_t{1});
                    const auto started = esp_timer_get_time();
                    const auto started_tick = xTaskGetTickCount();
                    execution::start(alarm_operation);
                    while (!alarm_fired.load(std::memory_order_acquire)) {
                        if (esp_timer_get_time() - started > 1'000'000)
                            std::abort();
                    }
                    // No blocking/yielding API between ISR return and this observation.
                    // A correct ISR yield lets the higher-priority worker run first.
                    const bool ran_before_resume = worker_ran.load();
                    if (xTaskGetTickCount() == started_tick) {
                        ++probe.observations;
                        probe.worker_ran_before_resume &= ran_before_resume;
                    }
                    wait_for_completion(completed);
                    wait_for_completion(alarm_done);
                    probe.all_completed &= completion.values.load() == 1 &&
                                           completion.errors.load() == 0 &&
                                           completion.stopped.load() == 0;
                }
            }
            release(probe.done);
            vTaskSuspend(nullptr);
        },
        "irq_yield_producer", 4096, &probe, tskIDLE_PRIORITY + 1, &producer, probe.core);
    REQUIRE(created == pdPASS); // No producer exists on failure.
    wait_for_completion(done);
    join_suspended_task(producer);

    REQUIRE(probe.observations == 8);
    REQUIRE(probe.all_completed);
    REQUIRE(probe.worker_ran_before_resume);
}
