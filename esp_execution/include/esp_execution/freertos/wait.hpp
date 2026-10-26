// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <optional>
#include <tuple>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_freertos_cpp/semaphore.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/value_types.hpp"

namespace esp::execution::freertos {
namespace detail::wait {

template <class Tuples> struct single_or_empty;
template <> struct single_or_empty<execution::detail::type_list<>> {
    using type = std::tuple<>;
};
template <class Tuple> struct single_or_empty<execution::detail::type_list<Tuple>> {
    using type = Tuple;
};

template <class... Tuples>
using single_or_empty_tuple =
    typename single_or_empty<execution::detail::unique_types_t<Tuples...>>::type;

template <class Sender>
using tuple_for =
    value_types_of_t<Sender, empty_env, execution::detail::decayed_tuple, single_or_empty_tuple>;

template <class Sender>
concept compatible =
    sender_in<Sender, empty_env> &&
    (execution::detail::count_completion_signatures_v<completion_signatures_of_t<Sender>,
                                                      set_error_t> == 0) &&
    execution::detail::all_selected_completion_signatures_satisfy_v<
        completion_signatures_of_t<Sender>, set_value_t,
        []<class... Values> {
            return ((std::is_nothrow_constructible_v<std::decay_t<Values>, Values> &&
                     std::is_nothrow_move_constructible_v<std::decay_t<Values>>) &&
                    ...);
        }> &&
    requires { typename tuple_for<Sender>; };

template <class Tuple> struct state {
    std::optional<Tuple> result;
    esp::freertos::binary_semaphore completed;
};

inline void notify(esp::freertos::binary_semaphore& completed) noexcept {
    if (xPortInIsrContext() != 0) {
        const auto released = completed.release_from_isr();
        contract_assert(released.released);
        if (released.task_woken) {
            portYIELD_FROM_ISR(pdTRUE);
        }
    } else {
        const bool released = completed.release();
        contract_assert(released);
    }
}

template <class Tuple> struct receiver {
    using receiver_concept = receiver_tag;
    state<Tuple>* completion;

    auto get_env() const noexcept -> empty_env { return {}; }

    void set_value(auto&&... values) && noexcept
        requires std::is_nothrow_constructible_v<Tuple, decltype(values)...>
    {
        completion->result.emplace(std::forward<decltype(values)>(values)...);
        // Completion may wake the waiter and destroy this receiver. Do not
        // access it or its state after signaling; notify only uses local data.
        notify(completion->completed);
    }

    void set_stopped() && noexcept { notify(completion->completed); }
};

template <class Sender>
concept waitable = compatible<Sender> &&
                   execution::detail::member_connectable<Sender, receiver<tuple_for<Sender>>>;

} // namespace detail::wait

[[nodiscard]] auto wait(detail::wait::waitable auto&& sender)
    -> std::optional<detail::wait::tuple_for<decltype(sender)>> pre(!xPortInIsrContext()) {
    detail::wait::state<detail::wait::tuple_for<decltype(sender)>> completion;
    auto operation = execution::connect(
        std::forward<decltype(sender)>(sender),
        detail::wait::receiver<detail::wait::tuple_for<decltype(sender)>>{&completion});
    execution::start(operation);
    // An aborted native wait is not sender completion. Retain the operation.
    while (!completion.completed.acquire()) {
    }
    return std::move(completion.result);
}

} // namespace esp::execution::freertos
