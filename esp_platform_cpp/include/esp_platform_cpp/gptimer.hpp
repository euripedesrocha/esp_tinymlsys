// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <sdkconfig.h>

#if CONFIG_GPTIMER_ISR_CACHE_SAFE
#error                                                                                             \
    "platform::gptimer does not yet support CONFIG_GPTIMER_ISR_CACHE_SAFE; disable it to use this adapter"
#endif

#include <chrono>
#include <concepts>
#include <cstdint>
#include <expected>
#include <functional>
#include <memory>
#include <type_traits>
#include <utility>

#include <driver/gptimer.h>
#include <freertos/FreeRTOS.h>

#include "esp_freertos_cpp/critical_section.hpp"

namespace esp::platform {

template <class Handler>
concept gptimer_alarm_handler =
    std::is_object_v<Handler> &&
    requires(Handler& handler, const gptimer_alarm_event_data_t& event) {
        { std::invoke(handler, event) } noexcept -> std::same_as<bool>;
    };

/** Owns one hardware GPTimer. Its ISR retains the owner's stable address. */
class gptimer {
    struct alarm_handler {
        void* state = nullptr;
        bool (*invoke)(void*, const gptimer_alarm_event_data_t&) noexcept = nullptr;
    };

public:
    gptimer() pre(!xPortInIsrContext());
    explicit gptimer(const gptimer_config_t& configuration) pre(!xPortInIsrContext())
        pre(configuration.resolution_hz > 0);
    ~gptimer();

    gptimer(const gptimer&) = delete ("the timer uniquely owns its peripheral");
    gptimer(gptimer&&) = delete ("the ISR and operations retain the timer's address");
    gptimer& operator=(const gptimer&) = delete ("the timer uniquely owns its peripheral");
    gptimer& operator=(gptimer&&) = delete ("the ISR and operations retain the timer's address");

    // As in IDF, stop only stops counting; a callback may still be executing when it returns.
    [[nodiscard]] auto start() noexcept -> std::expected<void, esp_err_t>;
    [[nodiscard]] auto stop() noexcept -> std::expected<void, esp_err_t>;
    [[nodiscard]] auto count() const noexcept -> std::expected<std::uint64_t, esp_err_t>;
    [[nodiscard]] auto set_count(std::uint64_t value) noexcept -> std::expected<void, esp_err_t>;
    [[nodiscard]] auto set_alarm(const gptimer_alarm_config_t& alarm) noexcept
        -> std::expected<void, esp_err_t>;
    [[nodiscard]] auto clear_alarm() noexcept -> std::expected<void, esp_err_t>;

    // Task-only. Prevent further starts until this returns. Pending callbacks
    // can finish (including updating an alarm), but must not restart counting.
    // The counter remains stopped and the resource can be reused.
    [[nodiscard]] auto stop_and_join() noexcept -> std::expected<void, esp_err_t>;

    [[nodiscard]] auto resolution_hz() const noexcept -> std::uint32_t { return resolution_hz_; }
    [[nodiscard]] static auto max_count() noexcept -> std::uint64_t;
    [[nodiscard]] auto ticks_for(std::chrono::nanoseconds duration) const noexcept -> std::uint64_t;

    // Bind only after construction or stop_and_join. The callable must outlive
    // its registration and all callbacks. Its bool requests an IDF ISR yield.
    void set_alarm_handler(gptimer_alarm_handler auto& handler) noexcept {
        using handler_type = std::remove_reference_t<decltype(handler)>;
        bind_handler(
            {.state = const_cast<void*>(static_cast<const volatile void*>(std::addressof(handler))),
             .invoke = [](void* state, const gptimer_alarm_event_data_t& event) noexcept {
                 return std::invoke(*static_cast<handler_type*>(state), event);
             }});
    }
    void clear_alarm_handler() noexcept { bind_handler({}); }

    void set_alarm_handler(const gptimer_alarm_handler auto&&) =
        delete ("the timer borrows its handler; provide an lvalue that outlives its callbacks");

private:
    void bind_handler(alarm_handler handler) noexcept;
    static bool on_alarm(gptimer_handle_t timer, const gptimer_alarm_event_data_t* event,
                         void* state) noexcept;

    gptimer_handle_t handle_ = nullptr;
    std::uint32_t resolution_hz_ = 0;
    mutable esp::freertos::spinlock mutex_;
    alarm_handler handler_;
    bool running_ = false;
    bool callbacks_joined_ = true;
    bool joining_ = false;
};

} // namespace esp::platform
