// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

namespace esp::execution::detail {

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

} // namespace esp::execution::detail
