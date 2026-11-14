// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/idf/gptimer_context.hpp"

#include <mutex>

namespace esp::execution::idf {

gptimer_context::gptimer_context(const gptimer_config_t& configuration) : timer_(configuration) {
    timer_.set_alarm_handler(handler_);
    const auto started = timer_.start();
    contract_assert(started.has_value());
}

gptimer_context::~gptimer_context() {
    contract_assert(!xPortInIsrContext());
    {
        std::lock_guard guard{mutex_};
        contract_assert(pending_ == nullptr);
    }
    const auto joined = timer_.stop_and_join();
    contract_assert(joined.has_value());
    timer_.clear_alarm_handler();
}

auto gptimer_context::submit(alarm_job& job, std::chrono::nanoseconds delay) noexcept
    -> std::expected<void, error> {
    std::lock_guard guard{mutex_};
    if (pending_ != nullptr)
        return std::unexpected(gptimer_schedule_error::busy);
    constexpr std::uint64_t ns_per_second = 1'000'000'000;
    const auto ns = delay.count() > 0 ? static_cast<std::uint64_t>(delay.count()) : 0;
    const auto rate = timer_.resolution_hz();
    const auto seconds = ns / ns_per_second;
    const auto fraction = ((ns % ns_per_second) * rate + ns_per_second - 1) / ns_per_second;
    const auto maximum = esp::platform::gptimer::max_count();
    if (seconds > maximum / rate || fraction > maximum - seconds * rate)
        return std::unexpected(gptimer_schedule_error::deadline_out_of_range);
    const auto rounded_ticks = seconds * rate + fraction;
    if (rounded_ticks == maximum)
        return std::unexpected(gptimer_schedule_error::deadline_out_of_range);
    // The current count does not reveal its fractional tick phase. One extra
    // tick ensures a positive delay cannot expire early. Immediate scheduling
    // also crosses the interrupt boundary.
    const auto ticks = rounded_ticks + 1;
    const auto now = timer_.count();
    if (!now)
        return std::unexpected(now.error());
    if (ticks > maximum - *now)
        return std::unexpected(gptimer_schedule_error::deadline_out_of_range);
    registered_at_ = *now;
    deadline_ = *now + ticks;
    pending_ = &job;
    const gptimer_alarm_config_t alarm{.alarm_count = deadline_, .reload_count = 0, .flags = {}};
    const auto armed = timer_.set_alarm(alarm);
    if (!armed) {
        pending_ = nullptr;
        return std::unexpected(armed.error());
    }
    const auto after = timer_.count();
    if (!after || *after < registered_at_) {
        const auto cleared = timer_.clear_alarm();
        contract_assert(cleared.has_value());
        pending_ = nullptr;
        if (!after)
            return std::unexpected(after.error());
        return std::unexpected(gptimer_schedule_error::deadline_out_of_range);
    }
    return {};
}

bool gptimer_context::handler::operator()(const gptimer_alarm_event_data_t&) noexcept {
    alarm_job job{};
    esp_err_t result = ESP_OK;
    {
        std::lock_guard guard{context->mutex_};
        if (context->pending_ == nullptr)
            return false;
        const auto now = context->timer_.count();
        if (now && *now >= context->registered_at_ && *now < context->deadline_) {
            // A stale pending interrupt must not complete a newly registered deadline.
            const gptimer_alarm_config_t alarm{
                .alarm_count = context->deadline_, .reload_count = 0, .flags = {}};
            const auto armed = context->timer_.set_alarm(alarm);
            if (armed)
                return false;
            result = armed.error();
        } else if (!now) {
            result = now.error();
        }
        job = *std::exchange(context->pending_, nullptr);
    }
    // Completion may destroy its operation and immediately arm the next one.
    job.complete(job.state, result);
    return false;
}

} // namespace esp::execution::idf
