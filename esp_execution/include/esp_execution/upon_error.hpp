// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include "esp_execution/completion.hpp"
#include "esp_execution/detail/continuation.hpp"

namespace esp::execution {

using upon_error_t = detail::continuation_adaptor<set_error_t>;

inline constexpr upon_error_t upon_error{};

} // namespace esp::execution
