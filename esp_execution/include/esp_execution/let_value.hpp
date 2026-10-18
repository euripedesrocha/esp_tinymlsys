// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
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
            return (std::is_nothrow_constructible_v<std::decay_t<Values>, Values> && ...) &&
                requires(Callable && callable, std::decay_t<Values> & ... values)
            {
                {
                    std::invoke(std::forward<Callable>(callable), values...)
                } noexcept -> sender_in<Env>;
            };
        }>;

template <class Signature, class Callable, class Env> struct result_signatures {
    using type = completion_signatures<Signature>;
};

template <class... Values, class Callable, class Env>
struct result_signatures<set_value_t(Values...), Callable, Env> {
    using child_sender = std::invoke_result_t<Callable&&, std::decay_t<Values>&...>;
    using type = completion_signatures_of_t<child_sender, Env>;
};

template <class Sender, movable_value Callable>
    requires let_value_compatible<Sender, Callable>
class sender {
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
            // Destroy the child before the values it may borrow.
            operation_state_storage<child_operation> child;

            explicit branch(auto&&... incoming)
                : values(std::forward<decltype(incoming)>(incoming)...) {}

            void start(operation& parent) noexcept {
                auto& child_state = child.emplace_from([&] {
                    return std::apply(
                        [&](auto&... stored) {
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

        template <class Signature> struct alternative {
            using type = std::monostate;
        };
        template <class... Values> struct alternative<set_value_t(Values...)> {
            using type = branch<std::tuple<std::decay_t<Values>...>>;
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
                auto& state = parent->branches_.template emplace<selected>(
                    std::forward<decltype(values)>(values)...);
                state.start(*parent);
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
        completion_storage_t<completion_signatures_of_t<Sender, env_of_t<Receiver>>, alternative>
            branches_;

    public:
        using operation_state_concept = operation_state_tag;

        operation(Sender source, Receiver receiver, Callable callable)
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

    // Compatibility with empty_env is a class invariant. Only new environments
    // introduce additional compatibility requirements.
    template <class Env>
        requires std::same_as<Env, empty_env> || let_value_compatible<Sender, Callable, Env>
    using completion_signatures =
        map_completion_signatures_t<completion_signatures_of_t<Sender, Env>, result_signatures,
                                    Callable, Env>;

    sender(Sender source, Callable callable)
        : source_(std::move(source)), callable_(std::move(callable)) {}

    decltype(auto) get_env() const noexcept { return execution::get_env(source_); }

    auto connect(receiver_for_sender<sender> auto receiver) && {
        return operation<decltype(receiver)>{std::move(source_), std::move(receiver),
                                             std::move(callable_)};
    }

    auto connect(receiver_for_sender<sender> auto receiver) const&
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
 * adaptor does not choose an execution context; callables must be nonthrowing.
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
