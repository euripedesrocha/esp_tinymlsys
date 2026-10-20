// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <functional>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>

#include "esp_execution/completion.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/detail/completion_storage.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/scheduler.hpp"
#include "esp_execution/sender_adaptor.hpp"

namespace esp::execution {
namespace detail::continues_on {

template <class... Arguments>
concept storable_completion_arguments =
    ((std::is_nothrow_constructible_v<std::decay_t<Arguments>, Arguments> &&
      std::is_nothrow_move_constructible_v<std::decay_t<Arguments>>) &&
     ...);

inline constexpr auto store_completion = []<class Tag, class... Arguments>
    requires storable_completion_arguments<Arguments...>
{ return std::type_identity<std::tuple<Tag, std::decay_t<Arguments>...>>{}; };

template <class Sender, class Scheduler, class Env = empty_env>
concept compatible = sender_in<Sender, Env> && scheduler<Scheduler> &&
                     sender_in<schedule_result_t<Scheduler>, Env> &&
                     all_completion_signatures_satisfy_v<
                         completion_signatures_of_t<Sender, Env>, []<class Signature> {
                             return completion_signature_transformable<Signature, store_completion>;
                         }>;

// The sender's class constraint has already validated empty_env.
template <class Env, class Sender, class Scheduler>
concept compatible_environment = std::same_as<Env, empty_env> || compatible<Sender, Scheduler, Env>;

template <class Sender, class Scheduler>
    requires compatible<Sender, Scheduler>
class sender {
    template <class Receiver> class operation {
        Receiver receiver_;
        completion_storage_t<completion_signatures_of_t<Sender, env_of_t<Receiver>>,
                             store_completion>
            result_;

        class scheduled_receiver {
        public:
            using receiver_concept = receiver_tag;
            operation* parent;

            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value() && noexcept { parent->deliver(); }
            void set_error(auto&& error) && noexcept {
                execution::set_error(std::move(parent->receiver_),
                                     std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { execution::set_stopped(std::move(parent->receiver_)); }
        };

        class upstream_receiver {
        public:
            using receiver_concept = receiver_tag;
            operation* parent;

            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value(auto&&... values) && noexcept {
                parent->transfer(set_value_t{}, std::forward<decltype(values)>(values)...);
            }
            void set_error(auto&& error) && noexcept {
                parent->transfer(set_error_t{}, std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { parent->transfer(set_stopped_t{}); }
        };

        using scheduled_operation = decltype(execution::connect(
            std::declval<schedule_result_t<Scheduler>>(), std::declval<scheduled_receiver>()));
        using upstream_operation =
            decltype(execution::connect(std::declval<Sender>(), std::declval<upstream_receiver>()));
        scheduled_operation scheduled_;
        upstream_operation upstream_;

        void transfer(auto tag, auto&&... values) noexcept {
            using result_type = std::tuple<decltype(tag), std::decay_t<decltype(values)>...>;
            result_.template emplace<result_type>(tag, std::forward<decltype(values)>(values)...);
            execution::start(scheduled_);
            // The destination may already have completed and destroyed this state.
        }

        void deliver() noexcept {
            std::visit(
                [this](auto& stored) noexcept {
                    if constexpr (!std::same_as<std::decay_t<decltype(stored)>, std::monostate>) {
                        // Detach the payload before invoking a receiver which may
                        // destroy this operation and its completion storage.
                        auto result = std::move(stored);
                        std::apply(
                            [this](auto tag, auto&... values) noexcept {
                                tag(std::move(receiver_), std::move(values)...);
                            },
                            result);
                    }
                },
                result_);
        }

    public:
        using operation_state_concept = operation_state_tag;

        operation(Sender source, Scheduler scheduler, Receiver receiver)
            : receiver_(std::move(receiver)),
              scheduled_(execution::connect(execution::schedule(std::move(scheduler)),
                                            scheduled_receiver{this})),
              upstream_(execution::connect(std::move(source), upstream_receiver{this})) {}

        operation(const operation&) = delete ("the operation owns its receiver and completion");
        operation(operation&&) = delete ("internal receivers retain this operation's address");
        operation&
        operator=(const operation&) = delete ("the operation owns its receiver and completion");
        operation&
        operator=(operation&&) = delete ("internal receivers retain this operation's address");

        void start() & noexcept { execution::start(upstream_); }
    };

public:
    using sender_concept = sender_tag;

    struct environment {
        Scheduler scheduler;

        auto query(get_completion_scheduler_t<set_value_t>) const noexcept -> Scheduler {
            return scheduler;
        }
    };

    template <compatible_environment<Sender, Scheduler> Env>
    using completion_signatures = completion_signatures_cat_t<
        map_completion_signatures_t<completion_signatures_of_t<Sender, Env>,
                                    []<class Tag, class... Arguments> {
                                        return std::type_identity<execution::completion_signatures<
                                            Tag(std::decay_t<Arguments>...)>>{};
                                    }>,
        remove_completion_signatures_for_t<
            completion_signatures_of_t<schedule_result_t<Scheduler>, Env>, set_value_t>>;

    sender(Sender source, Scheduler scheduler)
        : source_(std::move(source)), scheduler_(std::move(scheduler)) {}

    auto get_env() const noexcept -> environment { return {scheduler_}; }

    auto connect(receiver_for_sender<sender> auto receiver) && {
        return operation<decltype(receiver)>{std::move(source_), scheduler_, std::move(receiver)};
    }

    auto connect(receiver_for_sender<sender> auto receiver) const&
        requires std::copy_constructible<Sender>
    {
        return operation<decltype(receiver)>{source_, scheduler_, std::move(receiver)};
    }

private:
    Sender source_;
    Scheduler scheduler_;
};

} // namespace detail::continues_on

struct continues_on_t {
    auto operator()(sender auto&& source, scheduler auto scheduler) const
        -> detail::continues_on::sender<std::decay_t<decltype(source)>, decltype(scheduler)> {
        return {std::forward<decltype(source)>(source), std::move(scheduler)};
    }

    auto operator()(scheduler auto scheduler) const {
        return detail::sender_adaptor_binding{std::bind_back(*this, std::move(scheduler))};
    }
};

inline constexpr continues_on_t continues_on{};
} // namespace esp::execution
