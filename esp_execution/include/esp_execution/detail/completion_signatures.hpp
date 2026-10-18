// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstddef>
#include <type_traits>

#include "esp_execution/detail/meta.hpp"

namespace esp::execution {

struct set_value_t;
struct set_error_t;
struct set_stopped_t;

template <class... Signatures> struct completion_signatures {};

namespace detail {

template <class Type>
concept valid_completion_argument = std::is_object_v<Type> || std::is_reference_v<Type>;

template <class Signature> inline constexpr bool is_completion_signature = false;

template <class... Values>
inline constexpr bool is_completion_signature<set_value_t(Values...)> =
    (valid_completion_argument<Values> && ...);

template <class Error>
inline constexpr bool is_completion_signature<set_error_t(Error)> =
    valid_completion_argument<Error>;

template <> inline constexpr bool is_completion_signature<set_stopped_t()> = true;

template <class Type> inline constexpr bool is_completion_signatures = false;

template <class... Signatures>
inline constexpr bool is_completion_signatures<completion_signatures<Signatures...>> =
    (is_completion_signature<Signatures> && ...);

// Predicate is a callable with operator()<Signature>() returning a constant bool.
// An empty signature list satisfies every predicate; non-lists are rejected.
template <class SignatureList, auto Predicate>
inline constexpr bool all_completion_signatures_satisfy_v = false;

template <class... Signatures, auto Predicate>
inline constexpr bool
    all_completion_signatures_satisfy_v<completion_signatures<Signatures...>, Predicate> =
        (Predicate.template operator()<Signatures>() && ...);

template <class Signature, class SelectedTag, auto Predicate>
inline constexpr bool selected_completion_satisfies_v = true;

template <class SelectedTag, class... Arguments, auto Predicate>
inline constexpr bool
    selected_completion_satisfies_v<SelectedTag(Arguments...), SelectedTag, Predicate> =
        Predicate.template operator()<Arguments...>();

// Check argument packs only on the selected channel. Other channels never
// instantiate Predicate, so it need not accept their argument shapes.
template <class SignatureList, class SelectedTag, auto Predicate>
inline constexpr bool all_selected_completion_signatures_satisfy_v =
    all_completion_signatures_satisfy_v<SignatureList, []<class Signature> {
        return selected_completion_satisfies_v<Signature, SelectedTag, Predicate>;
    }>;

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

} // namespace detail
} // namespace esp::execution
