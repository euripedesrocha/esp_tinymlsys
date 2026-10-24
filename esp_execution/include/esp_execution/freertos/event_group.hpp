// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <optional>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>

#include "esp_execution/freertos/chrono.hpp"

namespace esp::execution::freertos {

enum class clear_on_exit : uint8_t { preserve, clear };

class event_group {
public:
    event_group() pre(!xPortInIsrContext()) {
        handle_ = xEventGroupCreate();
        contract_assert(handle_ != nullptr);
    }

    ~event_group() {
        // GCC 16 ICEs with pre() on this noexcept body; keep the check here.
        contract_assert(!xPortInIsrContext());
        vEventGroupDelete(handle_);
    }

    event_group(const event_group&) =
        delete ("copying would create multiple owners of one FreeRTOS event group");
    event_group(event_group&&) =
        delete ("ownership is fixed at construction; moved-from states are not supported");
    event_group& operator=(const event_group&) =
        delete ("copying would create multiple owners of one FreeRTOS event group");
    event_group& operator=(event_group&&) =
        delete ("ownership is fixed at construction; moved-from states are not supported");

    auto set(EventBits_t selected) noexcept -> EventBits_t pre(!xPortInIsrContext())
        pre(valid_bits(selected)) {
        return xEventGroupSetBits(handle_, selected);
    }

    auto clear(EventBits_t selected) noexcept -> EventBits_t pre(!xPortInIsrContext())
        pre(valid_bits(selected)) {
        return xEventGroupClearBits(handle_, selected);
    }

    [[nodiscard]] auto get_bits() const noexcept -> EventBits_t pre(!xPortInIsrContext()) {
        return xEventGroupGetBits(handle_);
    }

    [[nodiscard]] auto wait_any(EventBits_t selected, clear_on_exit clearing) noexcept
        -> std::optional<EventBits_t> pre(!xPortInIsrContext()) pre(valid_bits(selected))
            pre(selected != 0) {
        return wait(selected, clearing, wait_condition::any, portMAX_DELAY);
    }

    [[nodiscard]] auto wait_all(EventBits_t selected, clear_on_exit clearing) noexcept
        -> std::optional<EventBits_t> pre(!xPortInIsrContext()) pre(valid_bits(selected))
            pre(selected != 0) {
        return wait(selected, clearing, wait_condition::all, portMAX_DELAY);
    }

    [[nodiscard]] auto wait_any_for(EventBits_t selected, std::chrono::nanoseconds timeout,
                                    clear_on_exit clearing) noexcept
        -> std::optional<EventBits_t> pre(!xPortInIsrContext()) pre(valid_bits(selected))
            pre(selected != 0) {
        return wait(selected, clearing, wait_condition::any, to_ticks(timeout));
    }

    [[nodiscard]] auto wait_all_for(EventBits_t selected, std::chrono::nanoseconds timeout,
                                    clear_on_exit clearing) noexcept
        -> std::optional<EventBits_t> pre(!xPortInIsrContext()) pre(valid_bits(selected))
            pre(selected != 0) {
        return wait(selected, clearing, wait_condition::all, to_ticks(timeout));
    }

private:
    enum class wait_condition : uint8_t { any, all };

    static constexpr bool valid_bits(EventBits_t bits) noexcept {
        return (bits & ~EventBits_t{0x00ff'ffff}) == 0;
    }

    auto wait(EventBits_t requested, clear_on_exit clearing, wait_condition condition,
              TickType_t timeout) noexcept -> std::optional<EventBits_t> {
        const auto bits =
            xEventGroupWaitBits(handle_, requested, clearing == clear_on_exit::clear,
                                static_cast<BaseType_t>(condition == wait_condition::all), timeout);
        const bool satisfied = condition == wait_condition::all ? (bits & requested) == requested
                                                                : (bits & requested) != 0;
        if (satisfied) {
            return bits;
        }
        return std::nullopt;
    }

    EventGroupHandle_t handle_ = nullptr;
};

} // namespace esp::execution::freertos
