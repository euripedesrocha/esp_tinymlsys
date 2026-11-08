// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <exception>
#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_scheduler.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/detail/completion_storage.hpp"
#include "esp_execution/detail/operation_state_storage.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/sender_adaptor.hpp"

namespace esp::execution {
namespace detail::let_value {

template <class Sender, class Callable, class Env = empty_env>
concept let_value_compatible =
    sender_in<Sender, Env> &&
    all_selected_completion_signatures_satisfy_v<
        completion_signatures_of_t<Sender, Env>, set_value_t, []<class... Values> {
            return (std::constructible_from<std::decay_t<Values>, Values> && ...) &&
                requires(Callable && callable, std::decay_t<Values> & ... values)
            {
                {std::invoke(std::forward<Callable>(callable), values...)}->sender_in<Env>;
            };
        }>;

template <class Callable, class Receiver, class... Values>
inline constexpr bool binding_nothrow =
    (std::is_nothrow_constructible_v<std::decay_t<Values>, Values> && ...) &&
    std::is_nothrow_invocable_v<Callable&&, std::decay_t<Values>&...> && requires {
        {
            execution::connect(
                std::declval<std::invoke_result_t<Callable&&, std::decay_t<Values> & ...>>(),
                std::declval<Receiver>())
        } noexcept;
    };

template <execution::sender Sender, movable_value Callable> class sender {
    template <class Receiver> class operation {
        Receiver receiver_;
        Callable callable_;

        class child_receiver {
        public:
            using receiver_concept = receiver_tag;
            operation* parent;

            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value(auto&&... values) && noexcept {
                execution::set_value(std::move(parent->receiver_),
                                     std::forward<decltype(values)>(values)...);
            }
            void set_error(auto&& error) && noexcept {
                execution::set_error(std::move(parent->receiver_),
                                     std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { execution::set_stopped(std::move(parent->receiver_)); }
        };

        template <class Values> struct branch;

        template <class... Values> struct branch<std::tuple<Values...>> {
            std::tuple<Values...> values;
            using child_sender = std::invoke_result_t<Callable&&, Values&...>;
            using child_operation = decltype(execution::connect(std::declval<child_sender>(),
                                                                std::declval<child_receiver>()));
            static constexpr bool start_nothrow =
                std::is_nothrow_invocable_v<Callable&&, Values&...> &&
                noexcept(execution::connect(std::declval<child_sender>(),
                                            std::declval<child_receiver>()));
            // Destroy the child before the values it may borrow.
            operation_state_storage<child_operation> child;

            explicit branch(auto&&... incoming)
                : values(std::forward<decltype(incoming)>(incoming)...) {}

            void start(operation& parent) noexcept(start_nothrow) {
                auto& child_state = child.emplace_from([&]() noexcept(start_nothrow) {
                    return std::apply(
                        [&](auto&... stored) noexcept(start_nothrow) {
                            return execution::connect(
                                std::invoke(std::move(parent.callable_), stored...),
                                child_receiver{&parent});
                        },
                        values);
                });
                execution::start(child_state);
                // Completion may have destroyed parent and this branch.
            }
        };

        class upstream_receiver {
        public:
            using receiver_concept = receiver_tag;
            operation* parent;

            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value(auto&&... values) && noexcept {
                using selected = branch<std::tuple<std::decay_t<decltype(values)>...>>;
                constexpr bool nothrow =
                    binding_nothrow<Callable, child_receiver, decltype(values)...>;
                auto bind = [&]() noexcept(nothrow) {
                    auto& state = parent->branches_.template emplace<selected>(
                        std::forward<decltype(values)>(values)...);
                    state.start(*parent);
                };
                if constexpr (nothrow) {
                    bind();
                } else {
                    try {
                        bind();
                    } catch (...) {
                        execution::set_error(std::move(parent->receiver_),
                                             std::current_exception());
                    }
                }
            }
            void set_error(auto&& error) && noexcept {
                execution::set_error(std::move(parent->receiver_),
                                     std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { execution::set_stopped(std::move(parent->receiver_)); }
        };

        using upstream_operation =
            decltype(execution::connect(std::declval<Sender>(), std::declval<upstream_receiver>()));
        upstream_operation upstream_;
        // Branches die before upstream, so both input and source state remain
        // alive for any child-operation destructor.
        completion_storage_t<completion_signatures_of_t<Sender, env_of_t<Receiver>>,
                             []<class Tag, class... Values> {
                                 if constexpr (std::same_as<Tag, set_value_t>) {
                                     return std::type_identity<
                                         branch<std::tuple<std::decay_t<Values>...>>>{};
                                 } else {
                                     return std::type_identity<std::monostate>{};
                                 }
                             }>
            branches_;

    public:
        using operation_state_concept = operation_state_tag;

        operation(Sender source, Receiver receiver,
                  Callable callable) noexcept(std::is_nothrow_move_constructible_v<Receiver> &&
                                              std::is_nothrow_move_constructible_v<Callable> &&
                                              noexcept(execution::connect(std::declval<Sender>(),
                                                                          upstream_receiver{})))
            : receiver_(std::move(receiver)), callable_(std::move(callable)),
              upstream_(execution::connect(std::move(source), upstream_receiver{this})) {}

        operation(const operation&) = delete ("the operation owns its receiver and child state");
        operation(operation&&) = delete ("internal receivers retain this operation's address");
        operation&
        operator=(const operation&) = delete ("the operation owns its receiver and child state");
        operation&
        operator=(operation&&) = delete ("internal receivers retain this operation's address");

        void start() & noexcept { execution::start(upstream_); }
    };

public:
    using sender_concept = sender_tag;

    template <class Env>
        requires let_value_compatible<Sender, Callable, Env>
    using completion_signatures = map_completion_signatures_t<
        completion_signatures_of_t<Sender, Env>, []<class Tag, class... Values> {
            if constexpr (std::same_as<Tag, set_value_t>) {
                using child_sender = std::invoke_result_t<Callable&&, std::decay_t<Values>&...>;
                using errors = std::conditional_t<
                    binding_nothrow<Callable, connect_probe_receiver<Env>, Values...>,
                    execution::completion_signatures<>,
                    execution::completion_signatures<set_error_t(std::exception_ptr)>>;
                return std::type_identity<completion_signatures_cat_t<
                    completion_signatures_of_t<child_sender, Env>, errors>>{};
            } else {
                return std::type_identity<execution::completion_signatures<Tag(Values...)>>{};
            }
        }>;

    sender(Sender source, Callable callable)
        : source_(std::move(source)), callable_(std::move(callable)) {}

    decltype(auto) get_env() const noexcept {
        // The sender returned by the callable may deliver a value, error, or
        // stopped completion on a different execution resource. The source's
        // completion schedulers therefore cannot describe this sender's outcomes.
        return hide_completion_scheduler_attributes<set_value_t, set_error_t, set_stopped_t>(
            source_);
    }

    auto connect(receiver_for_sender<sender> auto receiver) && noexcept(
        noexcept(operation<decltype(receiver)>{std::declval<Sender>(), std::move(receiver),
                                               std::declval<Callable>()})) {
        return operation<decltype(receiver)>{std::move(source_), std::move(receiver),
                                             std::move(callable_)};
    }

    auto connect(receiver_for_sender<sender> auto receiver) const& noexcept(
        noexcept(operation<decltype(receiver)>{std::declval<const Sender&>(), std::move(receiver),
                                               std::declval<const Callable&>()}))
        requires std::copy_constructible<Sender> && std::copy_constructible<Callable>
    {
        return operation<decltype(receiver)>{source_, std::move(receiver), callable_};
    }

private:
    Sender source_;
    Callable callable_;
};

} // namespace detail::let_value

/**
 * Create a dependent operation after a value completion. The callable receives
 * references to decayed inputs retained in the operation state, not transient
 * completion arguments. Child state is destroyed before those inputs. This
 * adaptor does not choose an execution context. Exceptions from retaining inputs,
 * invoking the callable, or connecting its sender become exception_ptr errors.
 */
struct let_value_t {
    auto operator()(sender auto&& source, detail::movable_value auto&& callable) const
        -> detail::let_value::sender<std::decay_t<decltype(source)>,
                                     std::decay_t<decltype(callable)>> {
        return {std::forward<decltype(source)>(source), std::forward<decltype(callable)>(callable)};
    }

    constexpr auto operator()(detail::movable_value auto&& callable) const {
        return detail::sender_adaptor_binding{
            std::bind_back(*this, std::forward<decltype(callable)>(callable))};
    }
};

inline constexpr let_value_t let_value{};
} // namespace esp::execution
