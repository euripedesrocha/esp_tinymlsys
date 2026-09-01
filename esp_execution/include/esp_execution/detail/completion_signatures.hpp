// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>

#include "esp_execution/completion.hpp"
#include "esp_execution/detail/meta.hpp"

namespace esp::execution::detail {

template <class SignatureList, class Signature> struct append_unique_completion_signature;

template <class... ExistingSignatures, class Signature>
struct append_unique_completion_signature<completion_signatures<ExistingSignatures...>, Signature> {
    using type = std::conditional_t<(std::is_same_v<Signature, ExistingSignatures> || ...),
                                    completion_signatures<ExistingSignatures...>,
                                    completion_signatures<ExistingSignatures..., Signature>>;
};

template <class... SignaturesList>
using completion_signatures_cat_t =
    fold_left_lists_t<completion_signatures<>, append_unique_completion_signature,
                      SignaturesList...>;
} // namespace esp::execution::detail
