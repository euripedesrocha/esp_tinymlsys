// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <algorithm>
#include <concepts>
#include <initializer_list>
#include <meta>
#include <utility>
#include <vector>

namespace esp::execution::detail {

template <class... Types> struct type_list {};

consteval auto fold_types(std::meta::info list, auto initial,
                          std::invocable<decltype(initial), std::meta::info> auto operation) {
    return std::ranges::fold_left(std::meta::template_arguments_of(std::meta::dealias(list)),
                                  std::move(initial), std::move(operation));
}

inline constexpr auto append = [](std::vector<std::meta::info> result, std::meta::info type) {
    result.push_back(type);
    return result;
};

inline constexpr auto append_unique = [](std::vector<std::meta::info> result,
                                         std::meta::info type) {
    if (std::ranges::find(result, type) == result.end()) {
        result.push_back(type);
    }
    return result;
};

consteval auto concatenate_type_lists(std::meta::info target,
                                      std::initializer_list<std::meta::info> lists)
    -> std::meta::info {
    std::vector<std::meta::info> arguments;
    for (auto list : lists) {
        arguments = fold_types(list, std::move(arguments), append);
    }
    return std::meta::substitute(target, arguments);
}

consteval auto deduplicate_type_list(std::meta::info list) -> std::meta::info {
    list = std::meta::dealias(list);
    return std::meta::substitute(std::meta::template_of(list),
                                 fold_types(list, std::vector<std::meta::info>{}, append_unique));
}

template <class... Lists>
using type_list_cat_t = [:concatenate_type_lists(^^type_list, {
                                                                  ^^Lists...}):];

template <class... Types> using unique_types_t = [:deduplicate_type_list(^^type_list<Types...>):];

// Bind the argument pack before applying a possibly fixed-arity alias.
template <class... Arguments> struct meta_arguments {
    template <template <class...> class Template> using apply = Template<Arguments...>;
};

template <template <class...> class Template, class... Arguments>
using meta_apply_t = typename meta_arguments<Arguments...>::template apply<Template>;

template <template <class...> class Template, class... Arguments>
concept valid_template_application = requires { typename meta_apply_t<Template, Arguments...>; };

} // namespace esp::execution::detail
