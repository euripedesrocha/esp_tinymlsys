// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/idf/gptimer.hpp"

#include <limits>
#include <mutex>

#include <esp_ipc.h>
#include <freertos/task.h>
#include <hal/timer_ll.h>

namespace esp::execution::idf {
namespace {

auto driver_result(esp_err_t result) noexcept -> std::expected<void, esp_err_t> {
    if (result != ESP_OK)
        return std::unexpected(result);
    return {};
}

} // namespace

gptimer::gptimer()
    : gptimer(gptimer_config_t{
          .clk_src = GPTIMER_CLK_SRC_DEFAULT,
          .direction = GPTIMER_COUNT_UP,
          .resolution_hz = 1'000'000,
          .intr_priority = 0,
          .flags = {},
      }) {}

gptimer::gptimer(const gptimer_config_t& configuration) {
    const auto created = gptimer_new_timer(&configuration, &handle_);
    contract_assert(created == ESP_OK);
    const gptimer_event_callbacks_t callbacks{.on_alarm = on_alarm};
    const auto registered = gptimer_register_event_callbacks(handle_, &callbacks, this);
    contract_assert(registered == ESP_OK);
    const auto queried = gptimer_get_resolution(handle_, &resolution_hz_);
    contract_assert(queried == ESP_OK);
    contract_assert(resolution_hz_ > 0);
    const auto enabled = gptimer_enable(handle_);
    contract_assert(enabled == ESP_OK);
}

gptimer::~gptimer() {
    contract_assert(!xPortInIsrContext());
    const auto joined = stop_and_join();
    contract_assert(joined.has_value());
    const auto disabled = gptimer_disable(handle_);
    contract_assert(disabled == ESP_OK);
    const auto deleted = gptimer_del_timer(handle_);
    contract_assert(deleted == ESP_OK);
}

auto gptimer::max_count() noexcept -> std::uint64_t {
    return std::numeric_limits<std::uint64_t>::max() >> (64 - TIMER_LL_COUNTER_BIT_WIDTH);
}

auto gptimer::ticks_for(std::chrono::nanoseconds duration) const noexcept -> std::uint64_t {
    contract_assert(duration >= std::chrono::nanoseconds::zero());
    constexpr std::uint64_t nanoseconds_per_second = 1'000'000'000;
    const auto ns = static_cast<std::uint64_t>(duration.count());
    const auto seconds = ns / nanoseconds_per_second;
    const auto fraction = ns % nanoseconds_per_second;
    // The remainder product fits uint64_t even at the maximum uint32_t rate.
    const auto fractional_ticks =
        ((fraction * resolution_hz_) + nanoseconds_per_second - 1) / nanoseconds_per_second;
    const auto maximum = max_count();
    contract_assert(seconds <= maximum / resolution_hz_);
    const auto whole_ticks = seconds * resolution_hz_;
    contract_assert(fractional_ticks <= maximum - whole_ticks);
    return whole_ticks + fractional_ticks;
}

auto gptimer::start() noexcept -> std::expected<void, esp_err_t> {
    std::lock_guard guard{mutex_};
    contract_assert(!joining_);
    const auto result = gptimer_start(handle_);
    if (result == ESP_OK) {
        running_ = true;
        callbacks_joined_ = false;
    }
    return driver_result(result);
}

auto gptimer::stop() noexcept -> std::expected<void, esp_err_t> {
    std::lock_guard guard{mutex_};
    const auto result = gptimer_stop(handle_);
    if (result == ESP_OK)
        running_ = false;
    return driver_result(result);
}

auto gptimer::count() const noexcept -> std::expected<std::uint64_t, esp_err_t> {
    std::lock_guard guard{mutex_};
    std::uint64_t value = 0;
    const auto result = gptimer_get_raw_count(handle_, &value);
    if (result != ESP_OK)
        return std::unexpected(result);
    return value;
}

auto gptimer::set_count(std::uint64_t value) noexcept -> std::expected<void, esp_err_t> {
    contract_assert(value <= max_count());
    std::lock_guard guard{mutex_};
    const auto result = gptimer_set_raw_count(handle_, value);
    if (result == ESP_OK)
        callbacks_joined_ = false;
    return driver_result(result);
}

auto gptimer::set_alarm(const gptimer_alarm_config_t& alarm) noexcept
    -> std::expected<void, esp_err_t> {
    contract_assert(alarm.alarm_count <= max_count());
    contract_assert(alarm.reload_count <= max_count());
    std::lock_guard guard{mutex_};
    const auto result = gptimer_set_alarm_action(handle_, &alarm);
    if (result == ESP_OK) {
        callbacks_joined_ = false;
    }
    return driver_result(result);
}

auto gptimer::clear_alarm() noexcept -> std::expected<void, esp_err_t> {
    std::lock_guard guard{mutex_};
    const auto result = gptimer_set_alarm_action(handle_, nullptr);
    return driver_result(result);
}

auto gptimer::stop_and_join() noexcept -> std::expected<void, esp_err_t> {
    contract_assert(!xPortInIsrContext());
    {
        std::lock_guard guard{mutex_};
        contract_assert(!joining_);
        const auto result = gptimer_stop(handle_);
        if (result != ESP_OK) {
            return std::unexpected(result);
        }
        running_ = false;
        if (callbacks_joined_) {
            return {};
        }
        joining_ = true;
    }
    // Counting is stopped, so no new alarms can be generated. Let pending and
    // executing ISRs return before their borrowed handler state can be released.
#if !CONFIG_FREERTOS_UNICORE
    for (int core = 0; core < portNUM_PROCESSORS; ++core) {
        if (core != xPortGetCoreID()) {
            const auto result = esp_ipc_call_blocking(core, [](void*) {}, nullptr);
            if (result != ESP_OK) {
                std::lock_guard guard{mutex_};
                joining_ = false;
                return std::unexpected(result);
            }
        }
    }
#endif
    std::lock_guard guard{mutex_};
    joining_ = false;
    callbacks_joined_ = true;
    return {};
}

void gptimer::bind_handler(alarm_handler handler) noexcept {
    contract_assert(!xPortInIsrContext());
    std::lock_guard guard{mutex_};
    contract_assert(!running_ && !joining_ && callbacks_joined_);
    handler_ = handler;
}

bool gptimer::on_alarm(gptimer_handle_t, const gptimer_alarm_event_data_t* event,
                       void* state) noexcept {
    auto& self = *static_cast<gptimer*>(state);
    alarm_handler handler;
    {
        std::lock_guard guard{self.mutex_};
        handler = self.handler_;
    }
    if (handler.invoke != nullptr) {
        return handler.invoke(handler.state, *event);
    }
    return false;
}

} // namespace esp::execution::idf
