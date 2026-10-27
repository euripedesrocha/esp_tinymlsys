// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <freertos/FreeRTOS.h>

namespace esp::freertos {

class spinlock {
public:
    spinlock() noexcept = default;

    spinlock(const spinlock&) = delete ("a spinlock uniquely owns its native critical section");
    spinlock(spinlock&&) = delete ("a native critical section requires a stable address");
    spinlock&
    operator=(const spinlock&) = delete ("a spinlock uniquely owns its native critical section");
    spinlock&
    operator=(spinlock&&) = delete ("a native critical section requires a stable address");

    void lock() noexcept { portENTER_CRITICAL_SAFE(&mux_); }

#if defined(portTRY_ENTER_CRITICAL_SAFE)
    [[nodiscard]] auto try_lock() noexcept -> bool {
        return portTRY_ENTER_CRITICAL_SAFE(&mux_, 0) == pdPASS;
    }
#endif

    void unlock() noexcept { portEXIT_CRITICAL_SAFE(&mux_); }

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};

} // namespace esp::freertos
