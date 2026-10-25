// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <ratio>

#include <freertos/FreeRTOS.h>

namespace esp::execution::freertos {

using tick_duration = std::chrono::duration<std::int64_t, std::ratio<1, configTICK_RATE_HZ>>;

[[nodiscard]] constexpr auto to_ticks(std::chrono::nanoseconds timeout) noexcept -> TickType_t {
    const auto count = timeout <= std::chrono::nanoseconds::zero()
                           ? std::int64_t{0}
                           : std::chrono::ceil<tick_duration>(timeout).count();
    // Caller precondition: GCC 16 fails with pre() here during constexpr
    // evaluation or code generation. Check the rounded count before narrowing.
    contract_assert(static_cast<std::uint64_t>(count) < portMAX_DELAY);
    return static_cast<TickType_t>(count);
}

} // namespace esp::execution::freertos
