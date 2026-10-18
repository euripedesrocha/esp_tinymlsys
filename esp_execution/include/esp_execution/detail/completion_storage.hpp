// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>
#include <variant>

#include "esp_execution/completion.hpp"
#include "esp_execution/detail/meta.hpp"

namespace esp::execution::detail {

template <class Variant, class Type> struct append_unique_alternative;

template <class... Types, class Type>
struct append_unique_alternative<std::variant<Types...>, Type> {
    using type = std::conditional_t<(std::is_same_v<Type, Types> || ...), std::variant<Types...>,
                                    std::variant<Types..., Type>>;
};

template <class Signatures, template <class> class Alternative> struct completion_storage;

template <class... Signatures, template <class> class Alternative>
struct completion_storage<completion_signatures<Signatures...>, Alternative> {
    using type = fold_left_t<std::variant<std::monostate>, append_unique_alternative,
                             typename Alternative<Signatures>::type...>;
};

template <class Signatures, template <class> class Alternative>
using completion_storage_t = typename completion_storage<Signatures, Alternative>::type;

} // namespace esp::execution::detail
