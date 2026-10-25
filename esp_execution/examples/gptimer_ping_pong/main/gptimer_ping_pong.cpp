// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <contracts>
#include <cstdint>

#include <esp_log.h>
#include <esp_system.h>

#include "esp_execution/idf/gptimer_context.hpp"
#include "esp_execution.hpp"
#include "esp_execution/freertos/wait.hpp"
#include "esp_execution/freertos/task_context.hpp"

void handle_contract_violation(const std::contracts::contract_violation& violation) noexcept {
    const auto location = violation.location();
    ESP_LOGE("ping-pong", "Contract violation: %s at %s:%u:%u (%s)", violation.comment(),
             location.file_name(), static_cast<unsigned>(location.line()),
             static_cast<unsigned>(location.column()), location.function_name());
    esp_system_abort("Contract violation");
}

namespace {

namespace execution = esp::execution;

struct describe_error {
    auto operator()(esp_err_t error) const noexcept -> const char* {
        return esp_err_to_name(error);
    }
    auto operator()(execution::freertos::schedule_error error) const noexcept -> const char* {
        return error == execution::freertos::schedule_error::queue_full ? "Task queue full"
                                                                        : "Task queue timeout";
    }
    auto operator()(execution::idf::gptimer_schedule_error error) const noexcept -> const char* {
        return error == execution::idf::gptimer_schedule_error::busy
                   ? "Timer busy"
                   : "Timer deadline out of range";
    }
};

} // namespace

extern "C" void app_main() {
    execution::freertos::task_context ping{{.name = "ping", .priority = 5, .queue_capacity = 2}};
    execution::freertos::task_context pong{{.name = "pong", .priority = 5, .queue_capacity = 2}};
    execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                           .direction = GPTIMER_COUNT_UP,
                                           .resolution_hz = 1'000'000,
                                           .intr_priority = 0,
                                           .flags = {}}};
    constexpr auto interval = std::chrono::milliseconds{500};
    std::uint32_t round = 0;

    auto work = execution::idf::schedule_after(timer.get_scheduler(), interval) |
                execution::continues_on(ping.get_scheduler()) | execution::then([&]() noexcept {
                    const auto turn = ++round;
                    ESP_LOGI("ping-pong", "Ping %lu on %s", static_cast<unsigned long>(turn),
                             pcTaskGetName(nullptr));
                    return turn;
                }) |
                execution::let_value([&](std::uint32_t& turn) noexcept {
                    // turn is retained by let_value until this child completes.
                    return execution::idf::schedule_after(timer.get_scheduler(), interval) |
                           execution::continues_on(pong.get_scheduler()) |
                           execution::then([&turn]() noexcept {
                               ESP_LOGI("ping-pong", "Pong %lu on %s",
                                        static_cast<unsigned long>(turn), pcTaskGetName(nullptr));
                           });
                }) |
                execution::repeat() | execution::upon_error(describe_error{}) |
                execution::upon_stopped([]() noexcept { return "Task context stopped"; });

    // wait owns the connected operation while app_main blocks.
    const auto result = execution::freertos::wait(work);
    contract_assert(result.has_value());
    const auto [failure_message] = *result;
    ESP_LOGE("ping-pong", "Pipeline failed: %s", failure_message);
    esp_system_abort("Example pipeline terminated");
}
