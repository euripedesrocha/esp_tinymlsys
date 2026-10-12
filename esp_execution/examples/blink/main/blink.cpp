// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <chrono>

#include <contracts>
#include <cstdio>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "esp_execution/connect.hpp"
#include "esp_execution/just.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/then.hpp"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_system.h"
#include "led_strip.h"
#include "sdkconfig.h"

void handle_contract_violation(const std::contracts::contract_violation& violation) noexcept {
    const auto location = violation.location();
    char details[384];
    std::snprintf(details, sizeof(details), "Contract violation: %s at %s:%u:%u (%s)",
                  violation.comment(), location.file_name(), static_cast<unsigned>(location.line()),
                  static_cast<unsigned>(location.column()), location.function_name());
    esp_system_abort(details);
}

namespace {

namespace execution = esp::execution;

constexpr auto tag = "blink";
constexpr auto led_gpio = static_cast<gpio_num_t>(CONFIG_ESP_EXECUTION_BLINK_GPIO);
constexpr auto blink_interval = std::chrono::milliseconds{CONFIG_ESP_EXECUTION_BLINK_PERIOD_MS};
static_assert(pdMS_TO_TICKS(blink_interval.count()) > 0,
              "Blink interval must be at least one FreeRTOS tick");

auto configure_led() noexcept -> led_strip_handle_t {
    led_strip_config_t strip_config{};
    strip_config.strip_gpio_num = led_gpio;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;

    led_strip_rmt_config_t rmt_config{};
    rmt_config.resolution_hz = 10 * 1000 * 1000;

    led_strip_handle_t led = nullptr;
    const auto created = led_strip_new_rmt_device(&strip_config, &rmt_config, &led);
    contract_assert(created == ESP_OK);
    const auto cleared = led_strip_clear(led);
    contract_assert(cleared == ESP_OK);
    return led;
}

auto set_led(led_strip_handle_t led, bool level) noexcept -> esp_err_t {
    if (!level) {
        return led_strip_clear(led);
    }

    const auto result = led_strip_set_pixel(led, 0, 16, 16, 16);
    return result == ESP_OK ? led_strip_refresh(led) : result;
}

struct terminal_receiver {
    using receiver_concept = execution::receiver_tag;

    auto get_env() const noexcept -> execution::empty_env { return {}; }
};

} // namespace

extern "C" void app_main(void) {
    const auto led = configure_led();
    bool level = false;
    auto work = execution::just(blink_interval) |
                execution::then([led, &level](std::chrono::milliseconds interval) noexcept {
                    vTaskDelay(pdMS_TO_TICKS(interval.count()));
                    const bool next_level = !level;
                    if (const auto result = set_led(led, next_level); result != ESP_OK) {
                        ESP_LOGW(tag, "LED update failed: %s", esp_err_to_name(result));
                        return;
                    }
                    level = next_level;
                    ESP_LOGI(tag, "RGB LED is %s", level ? "on" : "off");
                }) |
                execution::repeat();
    auto operation = execution::connect(work, terminal_receiver{});
    execution::start(operation);
}
