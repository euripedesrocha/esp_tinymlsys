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

template <class Signature, class ExpectedTag>
inline constexpr bool completion_signature_matches_tag_v = false;

template <class ActualTag, class... Arguments, class ExpectedTag>
inline constexpr bool completion_signature_matches_tag_v<ActualTag(Arguments...), ExpectedTag> =
    std::is_same_v<ActualTag, ExpectedTag>;

template <class Signatures, class Tag> struct remove_completion_signatures_for;

template <class... Signatures, class Tag>
struct remove_completion_signatures_for<completion_signatures<Signatures...>, Tag> {
    using type = completion_signatures_cat_t<
        std::conditional_t<completion_signature_matches_tag_v<Signatures, Tag>,
                           completion_signatures<>, completion_signatures<Signatures>>...>;
};

template <class Signatures, class Tag>
using remove_completion_signatures_for_t =
    typename remove_completion_signatures_for<Signatures, Tag>::type;

template <class Signatures, class Tag> struct count_completion_signatures;

template <class... Signatures, class Tag>
struct count_completion_signatures<completion_signatures<Signatures...>, Tag>
    : std::integral_constant<std::size_t,
                             (static_cast<std::size_t>(
                                  completion_signature_matches_tag_v<Signatures, Tag>) +
                              ... + 0U)> {};

template <class Signatures, class Tag>
inline constexpr std::size_t count_completion_signatures_v =
    count_completion_signatures<Signatures, Tag>::value;
} // namespace esp::execution::detail
