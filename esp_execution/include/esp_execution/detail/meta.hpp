// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>

namespace esp::execution::detail {

template <class... Types> struct type_list {};

template <class List, class Type> struct append_type;
template <template <class...> class List, class... Types, class Type>
struct append_type<List<Types...>, Type> {
    using type = List<Types..., Type>;
};

template <class List, class Type> struct append_unique_type;
template <template <class...> class List, class... Types, class Type>
struct append_unique_type<List<Types...>, Type> {
    using type = std::conditional_t<(std::is_same_v<Type, Types> || ...), List<Types...>,
                                    List<Types..., Type>>;
};

template <class State, template <class, class> class Operation, class... Inputs> struct fold_left;

template <class State, template <class, class> class Operation> struct fold_left<State, Operation> {
    using type = State;
};

template <class State, template <class, class> class Operation, class First, class... Rest>
struct fold_left<State, Operation, First, Rest...>
    : fold_left<typename Operation<State, First>::type, Operation, Rest...> {};

template <class State, template <class, class> class Operation, class... Inputs>
using fold_left_t = typename fold_left<State, Operation, Inputs...>::type;

template <class State, template <class, class> class Operation, class... Lists>
struct fold_left_lists;

template <class State, template <class, class> class Operation>
struct fold_left_lists<State, Operation> {
    using type = State;
};

template <class State, template <class, class> class Operation, template <class...> class List,
          class... Elements, class... Rest>
struct fold_left_lists<State, Operation, List<Elements...>, Rest...>
    : fold_left_lists<fold_left_t<State, Operation, Elements...>, Operation, Rest...> {};

template <class State, template <class, class> class Operation, class... Lists>
using fold_left_lists_t = typename fold_left_lists<State, Operation, Lists...>::type;

template <class... Lists>
using type_list_cat_t = fold_left_lists_t<type_list<>, append_type, Lists...>;

template <class... Types>
using unique_types_t = fold_left_t<type_list<>, append_unique_type, Types...>;

// Bind the argument pack before applying a possibly fixed-arity alias.
template <class... Arguments> struct meta_arguments {
    template <template <class...> class Template> using apply = Template<Arguments...>;
};

template <template <class...> class Template, class... Arguments>
using meta_apply_t = typename meta_arguments<Arguments...>::template apply<Template>;

} // namespace esp::execution::detail
