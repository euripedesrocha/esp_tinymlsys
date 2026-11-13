// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <exception>
#include <type_traits>
#include <utility>

#include "esp_execution/completion.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/detail/completion_signatures.hpp"
#include "esp_execution/detail/operation_state_storage.hpp"
#include "esp_execution/operation_state.hpp"
#include "esp_execution/scheduler.hpp"

namespace esp::execution {
namespace detail {
template <class Env> struct starts_on_attributes {
    Env base;

    template <completion_tag Tag>
    void query(get_completion_scheduler_t<Tag>) const =
        delete ("starts_on does not guarantee a completion scheduler");

    decltype(auto) query(auto tag) const noexcept(noexcept(base.query(tag)))
        requires queryable_with<Env, decltype(tag)>
    {
        return base.query(tag);
    }
};

template <class Env, class Scheduler> struct scheduled_env {
    Env base;
    Scheduler scheduler;
    auto query(get_scheduler_t) const noexcept { return scheduler; }
    decltype(auto) query(auto tag) const noexcept(noexcept(base.query(tag)))
        requires queryable_with<Env, decltype(tag)>
    {
        return base.query(tag);
    }
};

template <class Source, class Scheduler> class starts_on_sender {
    template <class Receiver> class operation {
        Receiver receiver_;
        Scheduler scheduler_;
        Source source_;
        struct child_receiver {
            using receiver_concept = receiver_tag;
            operation* parent;
            auto get_env() const noexcept {
                return scheduled_env<env_of_t<Receiver>, Scheduler>{
                    execution::get_env(parent->receiver_), parent->scheduler_};
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
        using child_type = decltype(execution::connect(std::declval<Source>(), child_receiver{}));
        static constexpr bool child_connect_nothrow =
            noexcept(execution::connect(std::declval<Source>(), child_receiver{}));
        operation_state_storage<child_type> child_;
        void start_child() noexcept {
            auto connect_child = [this]() noexcept(child_connect_nothrow) {
                return execution::connect(std::move(source_), child_receiver{this});
            };
            child_type* child;
            if constexpr (child_connect_nothrow) {
                child = &child_.emplace_from(connect_child);
            } else {
                try {
                    child = &child_.emplace_from(connect_child);
                } catch (...) {
                    execution::set_error(std::move(receiver_), std::current_exception());
                    return;
                }
            }
            execution::start(*child);
        }
        struct schedule_receiver {
            using receiver_concept = receiver_tag;
            operation* parent;
            decltype(auto) get_env() const noexcept {
                return execution::get_env(parent->receiver_);
            }
            void set_value() && noexcept { parent->start_child(); }
            void set_error(auto&& error) && noexcept {
                execution::set_error(std::move(parent->receiver_),
                                     std::forward<decltype(error)>(error));
            }
            void set_stopped() && noexcept { execution::set_stopped(std::move(parent->receiver_)); }
        };
        using scheduled_type = decltype(execution::connect(
            execution::schedule(std::declval<Scheduler>()), schedule_receiver{}));
        scheduled_type scheduled_;

    public:
        using operation_state_concept = operation_state_tag;
        operation(Source source, Scheduler scheduler, Receiver receiver) noexcept(
            std::is_nothrow_move_constructible_v<Receiver> &&
            std::is_nothrow_copy_constructible_v<Scheduler> &&
            std::is_nothrow_move_constructible_v<Source> &&
            noexcept(execution::connect(execution::schedule(std::move(scheduler)),
                                        schedule_receiver{this})))
            : receiver_(std::move(receiver)), scheduler_(scheduler), source_(std::move(source)),
              scheduled_(execution::connect(execution::schedule(std::move(scheduler)),
                                            schedule_receiver{this})) {}
        operation(const operation&) = delete ("the operation owns its receiver and child states");
        operation(operation&&) = delete ("internal receivers retain this operation's address");
        void start() & noexcept { execution::start(scheduled_); }
    };

public:
    using sender_concept = sender_tag;
    template <class Env>
    using completion_signatures = execution::detail::completion_signatures_cat_t<
        execution::completion_signatures_of_t<Source, scheduled_env<Env, Scheduler>>,
        execution::detail::remove_completion_signatures_for_t<
            execution::completion_signatures_of_t<schedule_result_t<Scheduler>, Env>, set_value_t>,
        execution::completion_signatures<set_error_t(std::exception_ptr)>>;
    starts_on_sender(Scheduler scheduler,
                     Source source) noexcept(std::is_nothrow_copy_constructible_v<Scheduler> &&
                                             std::is_nothrow_move_constructible_v<Source>)
        : scheduler_(scheduler), source_(std::move(source)) {}
    auto get_env() const noexcept {
        return starts_on_attributes<env_of_t<Source>>{execution::get_env(source_)};
    }
    auto connect(receiver_for_sender<starts_on_sender> auto receiver) const& noexcept(
        noexcept(operation<decltype(receiver)>{source_, scheduler_, std::move(receiver)}))
        requires std::copy_constructible<Source>
    {
        return operation<decltype(receiver)>{source_, scheduler_, std::move(receiver)};
    }
    auto connect(receiver_for_sender<starts_on_sender> auto receiver) && noexcept(noexcept(
        operation<decltype(receiver)>{std::move(source_), scheduler_, std::move(receiver)})) {
        return operation<decltype(receiver)>{std::move(source_), scheduler_, std::move(receiver)};
    }

private:
    Scheduler scheduler_;
    Source source_;
};
} // namespace detail
struct starts_on_t {
    auto operator()(scheduler auto scheduler, sender auto source) const
        noexcept(noexcept(detail::starts_on_sender{scheduler, std::move(source)})) {
        return detail::starts_on_sender{scheduler, std::move(source)};
    }
};
inline constexpr starts_on_t starts_on{};
} // namespace esp::execution
