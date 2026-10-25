// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <contracts>

#include <esp_log.h>
#include <esp_system.h>
#include <led_strip.h>

#include "esp_execution/idf/gptimer_context.hpp"
#include "esp_execution.hpp"
#include "esp_execution/freertos/wait.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "sdkconfig.h"

// Application contract policy: report the violation and stop the application.
void handle_contract_violation(const std::contracts::contract_violation& violation) noexcept {
    const auto location = violation.location();
    ESP_LOGE("gptimer-blink", "Contract violation: %s at %s:%u:%u (%s)", violation.comment(),
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

auto configure_led() -> led_strip_handle_t {
    led_strip_config_t strip{};
    strip.strip_gpio_num = CONFIG_ESP_EXECUTION_BLINK_GPIO;
    strip.max_leds = 1;
    strip.led_model = LED_MODEL_WS2812;
    strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    led_strip_rmt_config_t rmt{};
    rmt.resolution_hz = 10'000'000;
    led_strip_handle_t led = nullptr;
    ESP_ERROR_CHECK(led_strip_new_rmt_device(&strip, &rmt, &led));
    ESP_ERROR_CHECK(led_strip_clear(led));
    return led;
}

auto set_led(led_strip_handle_t led, bool on) noexcept -> esp_err_t {
    if (!on)
        return led_strip_clear(led);
    const auto result = led_strip_set_pixel(led, 0, 16, 16, 16);
    return result == ESP_OK ? led_strip_refresh(led) : result;
}

} // namespace

extern "C" void app_main() {
    const auto led = configure_led();
    execution::freertos::task_context worker{
        {.name = "blink-worker", .priority = 5, .queue_capacity = 2}};
    execution::idf::gptimer_context timer{{.clk_src = GPTIMER_CLK_SRC_DEFAULT,
                                           .direction = GPTIMER_COUNT_UP,
                                           .resolution_hz = 1'000'000,
                                           .intr_priority = 0,
                                           .flags = {}}};
    constexpr auto interval = std::chrono::milliseconds{CONFIG_ESP_EXECUTION_BLINK_PERIOD_MS};
    bool on = false;

    auto work = execution::idf::schedule_after(timer.get_scheduler(), interval) |
                execution::continues_on(worker.get_scheduler()) | execution::then([&]() noexcept {
                    const bool next = !on;
                    if (const auto result = set_led(led, next); result != ESP_OK) {
                        ESP_LOGW("gptimer-blink", "LED update failed: %s", esp_err_to_name(result));
                        return;
                    }
                    on = next;
                    ESP_LOGI("gptimer-blink", "RGB LED is %s on %s", on ? "on" : "off",
                             pcTaskGetName(nullptr));
                }) |
                execution::repeat() | execution::upon_error(describe_error{}) |
                execution::upon_stopped([]() noexcept { return "Task context stopped"; });

    // wait owns the connected operation while app_main blocks.
    const auto result = execution::freertos::wait(work);
    contract_assert(result.has_value());
    const auto [failure_message] = *result;
    ESP_LOGE("gptimer-blink", "Pipeline failed: %s", failure_message);
    esp_system_abort("Example pipeline terminated");
}
