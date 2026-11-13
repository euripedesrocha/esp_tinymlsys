// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "esp_execution/concepts.hpp"
#include "esp_execution/detail/meta.hpp"

namespace esp::execution {
namespace detail {

template <class... Values> using decayed_tuple = std::tuple<std::decay_t<Values>...>;

struct empty_variant {
    empty_variant() = delete;
};

template <class Types> struct as_variant;
template <> struct as_variant<type_list<>> {
    using type = empty_variant;
};
template <class... Types> struct as_variant<type_list<Types...>> {
    using type = std::variant<Types...>;
};

template <class... Types>
using variant_or_empty = typename as_variant<unique_types_t<std::decay_t<Types>...>>::type;

template <class SelectedTag, class Signature, template <class...> class Tuple>
struct gather_signature;

template <class SelectedTag, class Tag, class... Arguments, template <class...> class Tuple>
    requires(!std::is_same_v<SelectedTag, Tag>)
struct gather_signature<SelectedTag, Tag(Arguments...), Tuple> {
    using type = type_list<>;
};

template <class Tag, class... Arguments, template <class...> class Tuple>
    requires valid_template_application<Tuple, Arguments...>
struct gather_signature<Tag, Tag(Arguments...), Tuple> {
    using type = type_list<meta_apply_t<Tuple, Arguments...>>;
};

template <template <class...> class Variant, class... Types>
auto apply_type_list(type_list<Types...>) -> std::type_identity<meta_apply_t<Variant, Types...>>;

template <class Tag, template <class...> class Tuple, template <class...> class Variant,
          class... Signatures>
auto gather_signatures(completion_signatures<Signatures...>) -> decltype(apply_type_list<Variant>(
    type_list_cat_t<typename gather_signature<Tag, Signatures, Tuple>::type...>{}));

} // namespace detail

// P2300R10 [exec.utils.cmplsigs]: collect the selected argument packs without
// applying the Tuple template to error or stopped completions. Custom Variant
// templates control their own handling of duplicates and empty collections.
template <class Sender, class Env = empty_env,
          template <class...> class Tuple = detail::decayed_tuple,
          template <class...> class Variant = detail::variant_or_empty>
    requires sender_in<Sender, Env>
using value_types_of_t = typename decltype(detail::gather_signatures<set_value_t, Tuple, Variant>(
    std::declval<completion_signatures_of_t<Sender, Env>>()))::type;

} // namespace esp::execution
