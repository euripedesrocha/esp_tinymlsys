// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

// IWYU pragma: begin_exports

// Core vocabulary and concepts.
#include "esp_execution/concepts.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/value_types.hpp"

// Customization operations, ordered by the sender/receiver lifecycle.
#include "esp_execution/completion.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/scheduler.hpp"

// Sender factories and adaptors.
#include "esp_execution/sender_adaptor.hpp"
#include "esp_execution/just.hpp"
#include "esp_execution/then.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/continues_on.hpp"
#include "esp_execution/starts_on.hpp"
#include "esp_execution/repeat.hpp"

// IWYU pragma: end_exports
