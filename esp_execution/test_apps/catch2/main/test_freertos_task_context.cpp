// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_execution.hpp"
#include "esp_execution/enum_flags.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "esp_freertos_cpp/event_group.hpp"
#include "esp_freertos_cpp/semaphore.hpp"
#include "freertos_task_test_support.hpp"

namespace {

namespace execution = esp::execution;
namespace execution_freertos = esp::execution::freertos;

using execution_freertos::submission_policy;
using namespace std::chrono_literals;
using esp::freertos::binary_semaphore;
using esp::freertos::clear_on_exit;
using esp::freertos::event_group;
using esp_execution_test::clear_notifications;
using esp_execution_test::completion_record;
using esp_execution_test::recording_receiver;
using esp_execution_test::wait_for_notifications;
using execution_freertos::schedule_error;
using execution_freertos::task_context;

enum class phase : EventBits_t {
    none = 0,
    worker_entered = 1 << 0,
    begin = 1 << 1,
    started = 1 << 2,
    done = 1 << 3,
};

using execution::enum_flags::operator&;

bool wait_for_flags(event_group& progress, phase flags) {
    return progress.wait_all_for(std::to_underlying(flags), 5s, clear_on_exit::preserve)
        .has_value();
}

void release_worker(binary_semaphore& gate) {
    const bool released = gate.release();
    contract_assert(released);
}

// Signalling completion can wake this task before the signalling API returns.
// Wait until the helper has stopped using borrowed state before deleting it.
void join_helper_task(TaskHandle_t task) {
    const auto started = xTaskGetTickCount();
    while (eTaskGetState(task) != eSuspended) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(5000)) {
            std::abort();
        }
        vTaskDelay(TickType_t{1});
    }
    vTaskDelete(task);
}

struct blocking_receiver {
    using receiver_concept = execution::receiver_tag;

    completion_record* record;
    event_group& progress;
    binary_semaphore& release;
    std::atomic_bool* timed_out;

    auto get_env() const noexcept -> execution::empty_env { return {}; }

    void set_value() && noexcept {
        progress.set(std::to_underlying(phase::worker_entered));
        if (!release.try_acquire_for(5s)) {
            timed_out->store(true);
        }
        recording_receiver{record}.set_value();
    }

    void set_error(schedule_error error) && noexcept {
        recording_receiver{record}.set_error(error);
    }
    void set_stopped() && noexcept { recording_receiver{record}.set_stopped(); }
};

struct destruction_record {
    void* operation = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    completion_record completion;
};

struct destroying_receiver {
    using receiver_concept = execution::receiver_tag;
    destruction_record* record;

    void set_value() && noexcept { destroy_operation().set_value(); }
    void set_error(schedule_error error) && noexcept { destroy_operation().set_error(error); }
    void set_stopped() && noexcept { destroy_operation().set_stopped(); }

private:
    auto destroy_operation() noexcept -> recording_receiver {
        const auto receiver = recording_receiver{&record->completion};
        auto* operation = std::exchange(record->operation, nullptr);
        record->destroy(operation);
        return receiver; // Do not access this receiver after its operation is destroyed.
    }
};

using scheduler_type = task_context::scheduler;
using schedule_sender = execution::schedule_result_t<scheduler_type>;
using schedule_operation = decltype(execution::connect(std::declval<schedule_sender>(),
                                                       std::declval<recording_receiver>()));

enum class admission_wait : uint8_t { bounded, indefinite };

auto waiting_policy(admission_wait choice) -> submission_policy {
    return choice == admission_wait::bounded ? submission_policy::wait_for(2s)
                                             : submission_policy::wait();
}

} // namespace

TEST_CASE("FreeRTOS scheduler models the public scheduling vocabulary", "[freertos][scheduler]") {
    STATIC_REQUIRE(execution::scheduler<scheduler_type>);
    STATIC_REQUIRE(execution::sender_in<schedule_sender>);
    STATIC_REQUIRE(execution::operation_state<schedule_operation>);
}

TEST_CASE("a FreeRTOS task executes many scheduled operations", "[freertos][scheduler]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    task_context context{{.name = "schedule_many", .stack_size = 4096, .queue_capacity = 4}};
    const auto scheduler = context.get_scheduler();
    const auto scheduler_copy = scheduler;
    std::atomic_int sequence = 0;
    completion_record first{.waiter = caller, .sequence = &sequence};
    completion_record second{.waiter = caller, .sequence = &sequence};

    recording_receiver first_receiver{&first};
    const recording_receiver second_receiver{&second};
    auto first_operation = execution::connect(execution::schedule(scheduler), first_receiver);
    auto second_operation = execution::connect(execution::schedule(scheduler), second_receiver);

    REQUIRE(first.values.load() == 0);
    REQUIRE(second.values.load() == 0);
    REQUIRE(scheduler == scheduler_copy);

    execution::start(first_operation);
    execution::start(second_operation);
    wait_for_notifications(2);

    REQUIRE(first.values.load() == 1);
    REQUIRE(second.values.load() == 1);
    REQUIRE(first.order.load() == 1);
    REQUIRE(second.order.load() == 2);
    REQUIRE(first.completion_task.load() != caller);
    REQUIRE(first.completion_task.load() == second.completion_task.load());
}

TEST_CASE("different FreeRTOS contexts are different execution resources",
          "[freertos][scheduler]") {
    task_context first{{.name = "resource_one", .stack_size = 4096, .queue_capacity = 1}};
    task_context second{{.name = "resource_two", .stack_size = 4096, .queue_capacity = 1}};

    REQUIRE(first.get_scheduler() != second.get_scheduler());
    const auto attributes = execution::get_env(execution::schedule(first.get_scheduler()));
    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(attributes) ==
            first.get_scheduler());
    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(attributes) !=
            second.get_scheduler());
    STATIC_REQUIRE_FALSE(
        std::invocable<decltype(execution::get_completion_scheduler<execution::set_error_t>),
                       const decltype(attributes)&>);
    STATIC_REQUIRE_FALSE(
        std::invocable<decltype(execution::get_completion_scheduler<execution::set_stopped_t>),
                       const decltype(attributes)&>);
}

TEST_CASE("continues_on transfers work from separate producer tasks to one consumer",
          "[freertos][continues_on][producers]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    task_context first{{.name = "producer_one", .queue_capacity = 1}};
    task_context second{{.name = "producer_two", .queue_capacity = 1}};
    task_context consumer{{.name = "consumer", .queue_capacity = 2}};
    std::atomic<TaskHandle_t> first_task = nullptr;
    std::atomic<TaskHandle_t> second_task = nullptr;
    completion_record first_result{.waiter = caller};
    completion_record second_result{.waiter = caller};
    auto make_work = [&](auto scheduler, auto& producer_task) {
        return execution::schedule(scheduler) | execution::then([&producer_task]() noexcept {
                   producer_task.store(xTaskGetCurrentTaskHandle());
                   return 42;
               }) |
               execution::continues_on(consumer.get_scheduler());
    };
    auto first_operation = execution::connect(make_work(first.get_scheduler(), first_task),
                                              recording_receiver{&first_result});
    auto second_operation = execution::connect(make_work(second.get_scheduler(), second_task),
                                               recording_receiver{&second_result});
    execution::start(first_operation);
    execution::start(second_operation);
    wait_for_notifications(2);
    REQUIRE(first_result.values.load() == 1);
    REQUIRE(second_result.values.load() == 1);
    REQUIRE(first_result.value.load() == 42);
    REQUIRE(second_result.value.load() == 42);
    REQUIRE(first_task.load() != second_task.load());
    REQUIRE(first_result.completion_task.load() == second_result.completion_task.load());
    REQUIRE(first_result.completion_task.load() != first_task.load());
    REQUIRE(first_result.completion_task.load() != second_task.load());
    REQUIRE(first_result.completion_task.load() != caller);
}

TEST_CASE("a FreeRTOS context accepts a null task name", "[freertos][scheduler]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    task_context context{{.name = nullptr, .stack_size = 4096, .queue_capacity = 1}};
    completion_record result{.waiter = caller};
    auto operation = execution::connect(execution::schedule(context.get_scheduler()),
                                        recording_receiver{&result});

    execution::start(operation);
    wait_for_notifications(1);

    REQUIRE(result.values.load() == 1);
}

TEST_CASE("then runs inline on the scheduled FreeRTOS task", "[freertos][scheduler][then]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    task_context context{{.name = "schedule_then", .stack_size = 4096, .queue_capacity = 1}};
    std::atomic<TaskHandle_t> callable_task = nullptr;
    completion_record result{.waiter = caller};
    auto work =
        execution::schedule(context.get_scheduler()) | execution::then([&callable_task]() noexcept {
            callable_task.store(xTaskGetCurrentTaskHandle());
            return 42;
        });
    auto operation = execution::connect(std::move(work), recording_receiver{&result});

    execution::start(operation);
    wait_for_notifications(1);

    REQUIRE(result.values.load() == 1);
    REQUIRE(result.value.load() == 42);
    REQUIRE(callable_task.load() != caller);
    REQUIRE(callable_task.load() == result.completion_task.load());
}

TEST_CASE("a full FreeRTOS task queue completes with an error", "[freertos][scheduler]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "schedule_full", .queue_capacity = 1}};
    const auto scheduler = context->get_scheduler();
    std::atomic_bool timed_out = false;
    completion_record blocked{.waiter = caller};
    completion_record queued{.waiter = caller};
    completion_record rejected{.waiter = caller};

    auto blocking_operation = execution::connect(
        execution::schedule(scheduler), blocking_receiver{&blocked, progress, release, &timed_out});
    auto queued_operation =
        execution::connect(execution::schedule(scheduler), recording_receiver{&queued});
    auto rejected_operation =
        execution::connect(execution::schedule(scheduler), recording_receiver{&rejected});

    execution::start(blocking_operation);
    const bool worker_entered = wait_for_flags(progress, phase::worker_entered);
    if (worker_entered) {
        execution::start(queued_operation);
        execution::start(rejected_operation);
    }
    // Observe inline rejection without waiting while the worker is gated. Always
    // release and join before an assertion can destroy the queued operation states.
    const auto inline_errors = rejected.errors.load();
    const auto rejection_task = rejected.completion_task.load();
    release_worker(release);
    context.reset();
    clear_notifications();

    REQUIRE(worker_entered);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(inline_errors == 1);
    REQUIRE(rejection_task == caller);
    REQUIRE(blocked.values.load() == 1);
    REQUIRE(queued.values.load() == 1);
    REQUIRE(rejected.values.load() == 0);
    REQUIRE(rejected.stopped.load() == 0);
}

TEST_CASE("waiting admission resumes when another source frees queue space",
          "[freertos][scheduler][admission]") {
    const auto choice = GENERATE(admission_wait::bounded, admission_wait::indefinite);
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "admission_wait", .queue_capacity = 1}};
    const auto scheduler = context->get_scheduler();
    const auto waiting = context->get_scheduler(waiting_policy(choice));
    // The completion scheduler must retain the source's admission choice.
    const auto recovered = execution::get_completion_scheduler<execution::set_value_t>(
        execution::get_env(execution::schedule(waiting)));
    REQUIRE(waiting == scheduler); // Same execution resource, different admission choices.
    REQUIRE(recovered == waiting);
    std::atomic_bool timed_out = false;
    std::atomic_int sequence = 0;
    completion_record blocked{.waiter = caller, .sequence = &sequence};
    completion_record queued{.waiter = caller, .sequence = &sequence};
    completion_record admitted{.waiter = caller, .sequence = &sequence};
    auto blocking = execution::connect(execution::schedule(scheduler),
                                       blocking_receiver{&blocked, progress, release, &timed_out});
    auto next = execution::connect(execution::schedule(scheduler), recording_receiver{&queued});
    // Exercise the same policy through a data-carrying continues_on handoff.
    auto operation = execution::connect(execution::just(42) | execution::continues_on(recovered),
                                        recording_receiver{&admitted});
    struct submission {
        decltype(operation)* operation_state;
        event_group& progress;
    } state{.operation_state = &operation, .progress = progress};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<submission*>(argument);
            if (!wait_for_flags(state.progress, phase::begin))
                std::abort();
            state.progress.set(std::to_underlying(phase::started));
            execution::start(*state.operation_state);
            state.progress.set(std::to_underlying(phase::done));
            vTaskSuspend(nullptr);
        },
        "waiting_source", 4096, &state, tskIDLE_PRIORITY + 1, &producer);
    REQUIRE(created == pdPASS); // Nothing has started if task creation fails.
    execution::start(blocking);
    if (!wait_for_flags(progress, phase::worker_entered))
        std::abort();
    execution::start(next);
    progress.set(std::to_underlying(phase::begin));
    if (!wait_for_flags(progress, phase::started))
        std::abort();
    const auto started = xTaskGetTickCount();
    while ((static_cast<phase>(progress.get_bits()) & phase::done) == phase::none &&
           eTaskGetState(producer) != eBlocked &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(500)) {
        vTaskDelay(TickType_t{1});
    }
    const bool waited = eTaskGetState(producer) == eBlocked &&
                        (static_cast<phase>(progress.get_bits()) & phase::done) == phase::none;
    release_worker(release);
    if (!wait_for_flags(progress, phase::done))
        std::abort();
    join_helper_task(producer);
    context.reset();
    clear_notifications();

    REQUIRE(waited);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(blocked.order.load() == 1);
    REQUIRE(queued.order.load() == 2);
    REQUIRE(admitted.order.load() == 3);
    REQUIRE(admitted.values.load() == 1);
    REQUIRE(admitted.value.load() == 42);
    REQUIRE(admitted.errors.load() == 0);
    REQUIRE(admitted.completion_task.load() == blocked.completion_task.load());
    REQUIRE(admitted.completion_task.load() != caller);
}

TEST_CASE("bounded admission times out without enqueueing the operation",
          "[freertos][scheduler][admission]") {
    const auto timeout =
        GENERATE(1ns, std::chrono::nanoseconds{1ms}, std::chrono::nanoseconds{15ms});
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "admission_timeout", .queue_capacity = 1}};
    std::atomic_bool timed_out = false;
    completion_record blocked{.waiter = caller};
    completion_record queued{.waiter = caller};
    completion_record rejected{.waiter = caller};
    auto blocking = execution::connect(execution::schedule(context->get_scheduler()),
                                       blocking_receiver{&blocked, progress, release, &timed_out});
    auto next = execution::connect(execution::schedule(context->get_scheduler()),
                                   recording_receiver{&queued});
    auto operation = execution::connect(
        execution::schedule(context->get_scheduler(submission_policy::wait_for(timeout))),
        recording_receiver{&rejected});
    execution::start(blocking);
    if (!wait_for_flags(progress, phase::worker_entered))
        std::abort();
    execution::start(next);
    const auto started = xTaskGetTickCount();
    execution::start(operation);
    const auto elapsed = xTaskGetTickCount() - started;
    const auto inline_errors = rejected.errors.load();
    release_worker(release);
    context.reset();
    clear_notifications();

    REQUIRE_FALSE(timed_out.load());
    // Independently calculate ceil(ns * tick_rate / 1e9), including sub-tick waits.
    const auto expected_ticks =
        (timeout.count() * configTICK_RATE_HZ + 999'999'999) / 1'000'000'000;
    REQUIRE(elapsed >= expected_ticks);
    REQUIRE(inline_errors == 1);
    REQUIRE(rejected.errors.load() == 1);
    REQUIRE(rejected.error.load() == schedule_error::queue_timeout);
    REQUIRE(rejected.completion_task.load() == caller);
    REQUIRE(rejected.values.load() == 0);
    REQUIRE(rejected.stopped.load() == 0);
    REQUIRE(queued.values.load() == 1);
}

TEST_CASE("nonblocking admission can schedule more work on its own worker",
          "[freertos][scheduler][admission]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "admission_worker", .queue_capacity = 1}};
    completion_record outer{.waiter = caller};
    completion_record inner{.waiter = caller};
    auto inner_operation = execution::connect(execution::schedule(context->get_scheduler()),
                                              recording_receiver{&inner});
    auto work =
        execution::schedule(context->get_scheduler()) |
        execution::then([&inner_operation]() noexcept { execution::start(inner_operation); });
    auto operation = execution::connect(std::move(work), recording_receiver{&outer});
    execution::start(operation);
    wait_for_notifications(2);
    context.reset();

    REQUIRE(outer.values.load() == 1);
    REQUIRE(inner.errors.load() == 0);
    REQUIRE(inner.values.load() == 1);
    REQUIRE(inner.stopped.load() == 0);
    REQUIRE(inner.completion_task.load() == outer.completion_task.load());
    REQUIRE(inner.completion_task.load() != caller);
}

TEST_CASE("finished contexts stop submissions regardless of admission choice",
          "[freertos][scheduler][admission]") {
    const auto choice = GENERATE(admission_wait::bounded, admission_wait::indefinite);
    clear_notifications();
    task_context context{{.name = "admission_finished", .queue_capacity = 1}};
    completion_record rejected{.waiter = xTaskGetCurrentTaskHandle()};
    auto operation =
        execution::connect(execution::schedule(context.get_scheduler(waiting_policy(choice))),
                           recording_receiver{&rejected});
    context.finish();
    execution::start(operation);
    wait_for_notifications(1);
    REQUIRE(rejected.stopped.load() == 1);
    REQUIRE(rejected.errors.load() == 0);
    REQUIRE(rejected.values.load() == 0);
}

TEST_CASE("a finished FreeRTOS context stops new schedules", "[freertos][scheduler]") {
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "schedule_stop", .queue_capacity = 2}};
    const auto scheduler = context->get_scheduler();
    std::atomic_bool timed_out = false;
    completion_record blocked{.waiter = caller};
    completion_record accepted{.waiter = caller};
    completion_record rejected{.waiter = caller};
    auto blocking_operation = execution::connect(
        execution::schedule(scheduler), blocking_receiver{&blocked, progress, release, &timed_out});
    auto accepted_operation =
        execution::connect(execution::schedule(scheduler), recording_receiver{&accepted});
    auto rejected_operation =
        execution::connect(execution::schedule(scheduler), recording_receiver{&rejected});

    execution::start(blocking_operation);
    const bool worker_entered = wait_for_flags(progress, phase::worker_entered);
    if (worker_entered) {
        execution::start(accepted_operation);
        // One slot remains for finish's stop marker while accepted work is gated.
        context->finish();
        context->finish(); // Repeated finish must not enqueue a second stop marker.
        execution::start(rejected_operation);
    }
    const auto accepted_before_release = accepted.values.load();
    const auto stopped_inline = rejected.stopped.load();
    release_worker(release);
    context.reset();
    clear_notifications();

    REQUIRE(worker_entered);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(accepted_before_release == 0);
    REQUIRE(stopped_inline == 1);
    REQUIRE(blocked.values.load() == 1);
    REQUIRE(accepted.values.load() == 1);
    REQUIRE(accepted.stopped.load() == 0);
    REQUIRE(accepted.completion_task.load() != caller);
    REQUIRE(rejected.values.load() == 0);
    REQUIRE(rejected.errors.load() == 0);
    REQUIRE(rejected.stopped.load() == 1);
    REQUIRE(rejected.completion_task.load() == caller);
}

TEST_CASE("FreeRTOS completion permits its receiver to destroy the scheduled operation",
          "[freertos][scheduler][lifetime]") {
    enum class completion_path : uint8_t { worker, full_queue, finished };
    const auto path =
        GENERATE(completion_path::worker, completion_path::full_queue, completion_path::finished);
    clear_notifications();
    auto* const caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "destroy_operation", .queue_capacity = 1}};
    const auto scheduler = context->get_scheduler();
    std::atomic_bool timed_out = false;
    completion_record blocked{.waiter = caller};
    completion_record next{.waiter = caller};
    destruction_record destroyed{.completion = {.waiter = caller}};
    auto blocking = execution::connect(execution::schedule(scheduler),
                                       blocking_receiver{&blocked, progress, release, &timed_out});
    auto following = execution::connect(execution::schedule(scheduler), recording_receiver{&next});

    if (path == completion_path::full_queue) {
        execution::start(blocking);
        // A failed gate cannot safely unwind an operation still used by the worker.
        if (!wait_for_flags(progress, phase::worker_entered))
            std::abort();
        execution::start(following);
    } else if (path == completion_path::finished) {
        context->finish();
    }

    using operation_type = decltype(execution::connect(execution::schedule(scheduler),
                                                       destroying_receiver{&destroyed}));
    auto* operation = new operation_type(
        execution::connect(execution::schedule(scheduler), destroying_receiver{&destroyed}));
    destroyed.operation = operation;
    destroyed.destroy = [](void* state) noexcept { delete static_cast<operation_type*>(state); };
    execution::start(*operation);
    wait_for_notifications(1);
    if (path == completion_path::full_queue) {
        release_worker(release);
    } else if (path == completion_path::worker) {
        // The worker must keep dispatching after the callback destroys its operation.
        execution::start(following);
        wait_for_notifications(1);
    }
    context.reset();
    clear_notifications();

    REQUIRE(destroyed.operation == nullptr);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(destroyed.completion.values.load() == (path == completion_path::worker ? 1 : 0));
    REQUIRE(destroyed.completion.errors.load() == (path == completion_path::full_queue ? 1 : 0));
    REQUIRE(destroyed.completion.stopped.load() == (path == completion_path::finished ? 1 : 0));
    if (path == completion_path::worker) {
        REQUIRE(destroyed.completion.completion_task.load() != caller);
        REQUIRE(next.completion_task.load() == destroyed.completion.completion_task.load());
    } else {
        REQUIRE(destroyed.completion.completion_task.load() == caller);
    }
    REQUIRE(next.values.load() == (path == completion_path::finished ? 0 : 1));
}

TEST_CASE("finishing or destroying a full FreeRTOS context drains accepted work",
          "[freertos][scheduler]") {
    enum class shutdown { finish, destroy };
    const auto action = GENERATE(shutdown::finish, shutdown::destroy);
    clear_notifications();
    const auto caller = xTaskGetCurrentTaskHandle();
    event_group progress;
    binary_semaphore release;
    std::optional<task_context> context{
        std::in_place, task_context::config{.name = "schedule_destroy", .queue_capacity = 1}};
    const auto scheduler = context->get_scheduler();
    std::atomic_bool timed_out = false;
    completion_record blocked{.waiter = caller};
    completion_record accepted{.waiter = caller};
    auto blocking_operation = execution::connect(
        execution::schedule(scheduler), blocking_receiver{&blocked, progress, release, &timed_out});
    auto accepted_operation =
        execution::connect(execution::schedule(scheduler), recording_receiver{&accepted});

    struct destruction_state {
        std::optional<task_context>* context;
        event_group& progress;
        shutdown action;
    } state{.context = &context, .progress = progress, .action = action};
    TaskHandle_t destroyer = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<destruction_state*>(argument);
            if (!wait_for_flags(state.progress, phase::begin))
                std::abort();
            state.progress.set(std::to_underlying(phase::started));
            if (state.action == shutdown::finish) {
                state.context->value().finish();
            } else {
                state.context->reset();
            }
            state.progress.set(std::to_underlying(phase::done));
            // Keep the handle valid while the test inspects the task state.
            vTaskSuspend(nullptr);
        },
        "context_destroyer", 4096, &state, tskIDLE_PRIORITY + 1, &destroyer);
    REQUIRE(created == pdPASS); // No operations have started if task creation fails.

    execution::start(blocking_operation);
    const bool worker_entered = wait_for_flags(progress, phase::worker_entered);
    if (worker_entered) {
        execution::start(accepted_operation);
    }
    // The worker is gated and the only queue slot is occupied. Both finish and
    // destruction must wait for queue space before they can insert a stop marker.
    progress.set(std::to_underlying(phase::begin));
    const bool destroyer_started = wait_for_flags(progress, phase::started);
    const auto started = xTaskGetTickCount();
    while ((static_cast<phase>(progress.get_bits()) & phase::done) == phase::none &&
           eTaskGetState(destroyer) != eBlocked &&
           xTaskGetTickCount() - started < pdMS_TO_TICKS(1000)) {
        vTaskDelay(TickType_t{1});
    }
    const bool destruction_waited =
        eTaskGetState(destroyer) == eBlocked &&
        (static_cast<phase>(progress.get_bits()) & phase::done) == phase::none;
    const auto accepted_before_release = accepted.values.load();
    release_worker(release);
    const bool destroyed = wait_for_flags(progress, phase::done);
    // A failed join cannot safely unwind operation states still used by a task.
    if (!destroyed)
        std::abort();
    join_helper_task(destroyer);
    context.reset();
    clear_notifications();

    REQUIRE(worker_entered);
    REQUIRE(destroyer_started);
    REQUIRE(destruction_waited);
    REQUIRE_FALSE(timed_out.load());
    REQUIRE(accepted_before_release == 0);
    REQUIRE(blocked.values.load() == 1);
    REQUIRE(accepted.values.load() == 1);
    REQUIRE(accepted.errors.load() == 0);
    REQUIRE(accepted.stopped.load() == 0);
    REQUIRE(accepted.completion_task.load() == blocked.completion_task.load());
    REQUIRE(accepted.completion_task.load() != caller);
}
