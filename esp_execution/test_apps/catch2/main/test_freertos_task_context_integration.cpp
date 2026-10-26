// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <utility>

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

} // namespace

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
