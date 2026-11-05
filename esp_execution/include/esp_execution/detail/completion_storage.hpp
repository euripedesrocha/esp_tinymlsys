// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <variant>

#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/detail/meta.hpp"

namespace esp::execution::detail {

template <class Signatures, auto Transform> struct completion_storage;

template <class... Signatures, auto Transform>
    requires(completion_signature_transformable<Signatures, Transform> && ...)
struct completion_storage<completion_signatures<Signatures...>, Transform> {
    using type = [:deduplicate_type_list(
                       ^^std::variant<std::monostate,
                                      apply_completion_signature_t<Signatures, Transform>...>):];
};

template <class Signatures, auto Transform>
using completion_storage_t = typename completion_storage<Signatures, Transform>::type;

} // namespace esp::execution::detail
