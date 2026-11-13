// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <exception>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include <catch2/catch_test_macros.hpp>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_execution.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "esp_execution_test/freertos.hpp"
#include "esp_execution_test/execution.hpp"

namespace {

namespace execution = esp::execution;
namespace execution_freertos = esp::execution::freertos;

using esp_execution_test::clear_notifications;
using esp_execution_test::completion_record;
using esp_execution_test::recording_receiver;
using esp_execution_test::wait_for_notifications;
using execution_freertos::schedule_error;
using execution_freertos::task_context;

struct repeat_error_handler {
    std::atomic_int* handled_error;
    std::atomic<TaskHandle_t>* handler_task;

    void operator()(int error) const noexcept {
        handled_error->store(error);
        handler_task->store(xTaskGetCurrentTaskHandle());
    }

    void operator()(schedule_error) const noexcept {
        handled_error->store(-1);
        handler_task->store(xTaskGetCurrentTaskHandle());
    }
};

struct environment_scheduler_sender {
    using sender_concept = execution::sender_tag;
    std::atomic<TaskHandle_t>* connected_on;
    std::atomic<TaskHandle_t>* started_on;

    template <class Env>
    using completion_signatures = execution::completion_signatures<execution::set_value_t(
        decltype(execution::get_scheduler(std::declval<const Env&>())))>;

    auto connect(execution::receiver auto receiver) const {
        connected_on->store(xTaskGetCurrentTaskHandle());
        auto scheduler = execution::get_scheduler(execution::get_env(receiver));
        auto work =
            execution::just() | execution::then([scheduler, started_on = started_on]() noexcept {
                started_on->store(xTaskGetCurrentTaskHandle());
                return scheduler;
            });
        return execution::connect(std::move(work), std::move(receiver));
    }
};

struct scheduler_recording_receiver : recording_receiver {
    task_context::scheduler expected_scheduler;
    std::atomic_bool* matched;
    using recording_receiver::set_error;

    void set_value(task_context::scheduler scheduler) && noexcept {
        matched->store(scheduler == expected_scheduler);
        std::move(static_cast<recording_receiver&>(*this)).set_value();
    }
    void set_error(std::exception_ptr) && noexcept {
        record->completion_task.store(xTaskGetCurrentTaskHandle());
        record->errors.fetch_add(1);
        xTaskNotifyGive(record->waiter);
    }
};

} // namespace

TEST_CASE("starts_on connects and starts its child on the FreeRTOS context task",
          "[freertos][scheduler][starts_on][environment]") {
    clear_notifications();
    auto* const caller = xTaskGetCurrentTaskHandle();
    task_context context{{.name = "starts_on", .stack_size = 4096, .queue_capacity = 1}};
    std::atomic<TaskHandle_t> connected_on = nullptr;
    std::atomic<TaskHandle_t> started_on = nullptr;
    std::atomic_bool matched = false;
    completion_record completion{.waiter = caller};

    auto work = execution::starts_on(context.get_scheduler(),
                                     environment_scheduler_sender{&connected_on, &started_on});
    STATIC_REQUIRE(std::same_as<execution::value_types_of_t<decltype(work)>,
                                std::variant<std::tuple<task_context::scheduler>>>);
    auto operation = execution::connect(
        std::move(work),
        scheduler_recording_receiver{{&completion}, context.get_scheduler(), &matched});

    execution::start(operation);
    wait_for_notifications(1);

    REQUIRE(completion.values.load() == 1);
    REQUIRE(completion.errors.load() == 0);
    REQUIRE(completion.stopped.load() == 0);
    REQUIRE(matched.load());
    REQUIRE(connected_on.load() != caller);
    REQUIRE(connected_on.load() == started_on.load());
    REQUIRE(started_on.load() == completion.completion_task.load());
}

TEST_CASE("repeat and its terminal error handler stay on the FreeRTOS context task",
          "[freertos][scheduler][repeat][upon_error]") {
    clear_notifications();
    constexpr int value_iterations = 3;
    auto* const caller = xTaskGetCurrentTaskHandle();
    task_context context{{.name = "repeat_schedule", .stack_size = 4096, .queue_capacity = 1}};
    int iterations = 0;
    std::atomic_int handled_error = 0;
    std::atomic<TaskHandle_t> iteration_task = nullptr;
    std::atomic<TaskHandle_t> handler_task = nullptr;
    completion_record completion{.waiter = caller};

    auto iteration = execution::schedule(context.get_scheduler()) |
                     execution::let_value([&iterations, &iteration_task]() noexcept {
                         iteration_task.store(xTaskGetCurrentTaskHandle());
                         return esp_execution_test::iteration_sender{
                             .iterations_started = &iterations,
                             .iterations_before_terminal = value_iterations,
                             .terminal = esp_execution_test::terminate_as::error};
                     });
    auto work = std::move(iteration) | execution::repeat() |
                execution::continues_on(context.get_scheduler()) |
                execution::upon_error(repeat_error_handler{&handled_error, &handler_task});
    auto operation = execution::connect(std::move(work), recording_receiver{&completion});

    execution::start(operation);
    wait_for_notifications(1);

    REQUIRE(iterations == value_iterations + 1);
    REQUIRE(handled_error.load() == value_iterations + 1);
    REQUIRE(completion.values.load() == 1);
    REQUIRE(completion.errors.load() == 0);
    REQUIRE(iteration_task.load() != caller);
    REQUIRE(iteration_task.load() == handler_task.load());
    REQUIRE(handler_task.load() == completion.completion_task.load());
}
