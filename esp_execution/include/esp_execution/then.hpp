// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "esp_execution/completion.hpp"
#include "esp_execution/detail/continuation.hpp"

namespace esp::execution {

using then_t = detail::continuation_adaptor<set_value_t>;

inline constexpr then_t then{};

} // namespace esp::execution
