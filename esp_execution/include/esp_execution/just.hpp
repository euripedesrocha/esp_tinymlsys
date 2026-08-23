// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <tuple>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"

namespace esp::execution {

namespace detail {

template <class Completion, class... Values> class just_sender {
    template <class Receiver> class operation {
    public:
        using operation_state_concept = operation_state_tag;

        constexpr operation(auto&& receiver, auto&& values)
            requires std::constructible_from<Receiver, decltype(receiver)> &&
                         std::constructible_from<std::tuple<Values...>, decltype(values)>
            : receiver_(std::forward<decltype(receiver)>(receiver)),
              values_(std::forward<decltype(values)>(values)) {}

        operation(const operation&) = delete ("operation states cannot be copied after connection");
        operation(operation&&) = delete ("operation states cannot be moved after connection");
        operation& operator=(const operation&) =
            delete ("operation states cannot be copy-assigned after connection");
        operation& operator=(operation&&) =
            delete ("operation states cannot be move-assigned after connection");

        constexpr void start() & noexcept {
            std::apply(
                [this](auto&... values) noexcept {
                    Completion{}(std::move(receiver_), std::move(values)...);
                },
                values_);
        }

    private:
        Receiver receiver_;
        std::tuple<Values...> values_;
    };

public:
    using sender_concept = sender_tag;

    template <class Env>
    using completion_signatures = esp::execution::completion_signatures<Completion(Values...)>;

    constexpr explicit just_sender(auto&&... values)
        requires std::constructible_from<std::tuple<Values...>, decltype(values)...>
        : values_(std::forward<decltype(values)>(values)...) {}

    constexpr auto connect(receiver auto&& receiver) && {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{std::forward<decltype(receiver)>(receiver),
                                        std::move(values_)};
    }

    constexpr auto connect(receiver auto&& receiver) const&
        requires(std::copy_constructible<Values> && ...)
    {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{std::forward<decltype(receiver)>(receiver), values_};
    }

private:
    std::tuple<Values...> values_;
};

} // namespace detail

struct just_t {
    constexpr auto operator()(detail::movable_value auto&&... values) const {
        return detail::just_sender<set_value_t, std::decay_t<decltype(values)>...>{
            std::forward<decltype(values)>(values)...};
    }
};

struct just_error_t {
    constexpr auto operator()(detail::movable_value auto&& error) const {
        return detail::just_sender<set_error_t, std::decay_t<decltype(error)>>{
            std::forward<decltype(error)>(error)};
    }
};

struct just_stopped_t {
    constexpr auto operator()() const { return detail::just_sender<set_stopped_t>{}; }
};

inline constexpr just_t just{};
inline constexpr just_error_t just_error{};
inline constexpr just_stopped_t just_stopped{};

} // namespace esp::execution
