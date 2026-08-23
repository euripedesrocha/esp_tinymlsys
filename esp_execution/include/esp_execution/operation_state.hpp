// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "esp_execution/concepts.hpp"

namespace esp::execution {

struct start_t {
    constexpr void operator()(operation_state auto& operation) const noexcept { operation.start(); }
};

inline constexpr start_t start{};

} // namespace esp::execution
