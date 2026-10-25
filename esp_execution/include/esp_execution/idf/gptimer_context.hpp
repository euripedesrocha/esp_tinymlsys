// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <chrono>
#include <cstdint>
#include <expected>
#include <type_traits>
#include <utility>
#include <variant>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_freertos_cpp/critical_section.hpp"
#include "esp_execution/idf/gptimer.hpp"
#include "esp_execution/scheduler.hpp"

namespace esp::execution::idf {

enum class gptimer_schedule_error : uint8_t { busy, deadline_out_of_range };

/** A continuously running timer with one outstanding deadline.
 * Handles and operations borrow the context. Stop producers and complete the
 * pending operation before destruction. Success runs in the timer ISR; errors
 * run where detected. Receivers must support those execution contexts.
 */
class gptimer_context {
    using error = std::variant<esp_err_t, gptimer_schedule_error>;
    struct alarm_job {
        void* state;
        void (*complete)(void*, esp_err_t) noexcept;
    };

public:
    class scheduler {
    public:
        class schedule_sender {
            template <class Receiver> class operation {
            public:
                using operation_state_concept = execution::operation_state_tag;
                operation(gptimer_context& context, std::chrono::nanoseconds delay,
                          Receiver receiver)
                    : context_(context), delay_(delay), receiver_(std::move(receiver)) {}
                operation(const operation&) = delete ("an alarm uniquely owns its receiver");
                operation(operation&&) = delete ("the context retains the operation's address");
                operation&
                operator=(const operation&) = delete ("an alarm uniquely owns its receiver");
                operation&
                operator=(operation&&) = delete ("the context retains the operation's address");

                void start() & noexcept {
                    const auto result = context_.submit(job_, delay_);
                    if (!result)
                        std::visit(
                            [this](auto failure) noexcept {
                                execution::set_error(std::move(receiver_), failure);
                            },
                            result.error());
                }

            private:
                static void complete(void* state, esp_err_t result) noexcept {
                    auto& self = *static_cast<operation*>(state);
                    if (result == ESP_OK)
                        execution::set_value(std::move(self.receiver_));
                    else
                        execution::set_error(std::move(self.receiver_), result);
                }
                gptimer_context& context_;
                std::chrono::nanoseconds delay_;
                Receiver receiver_;
                alarm_job job_{this, complete};
            };

        public:
            using sender_concept = execution::sender_tag;
            template <class Env>
            using completion_signatures =
                execution::completion_signatures<set_value_t(), set_error_t(esp_err_t),
                                                 set_error_t(gptimer_schedule_error)>;
            struct environment {
                gptimer_context* context;
                auto query(get_completion_scheduler_t<set_value_t>) const noexcept -> scheduler {
                    return context->get_scheduler();
                }
            };
            schedule_sender(gptimer_context& context, std::chrono::nanoseconds delay) noexcept
                : context_(&context), delay_(delay) {}
            auto get_env() const noexcept -> environment { return {context_}; }
            auto
            connect(execution::detail::receiver_for_sender<schedule_sender> auto&& receiver) const {
                return operation<std::decay_t<decltype(receiver)>>{
                    *context_, delay_, std::forward<decltype(receiver)>(receiver)};
            }

        private:
            gptimer_context* context_;
            std::chrono::nanoseconds delay_;
        };

        using scheduler_concept = execution::scheduler_tag;
        auto schedule() const noexcept -> schedule_sender { return {*context_, {}}; }
        auto schedule_after(std::chrono::nanoseconds delay) const noexcept -> schedule_sender {
            return {*context_, delay};
        }
        friend bool operator==(const scheduler&, const scheduler&) = default;

    private:
        friend class gptimer_context;
        explicit scheduler(gptimer_context& context) noexcept : context_(&context) {}
        gptimer_context* context_;
    };

    explicit gptimer_context(const gptimer_config_t& configuration) pre(!xPortInIsrContext())
        pre(configuration.direction == GPTIMER_COUNT_UP);
    ~gptimer_context();
    gptimer_context(const gptimer_context&) = delete ("a context uniquely owns its timer");
    gptimer_context(gptimer_context&&) = delete ("scheduler handles retain the context's address");
    gptimer_context&
    operator=(const gptimer_context&) = delete ("a context uniquely owns its timer");
    gptimer_context&
    operator=(gptimer_context&&) = delete ("scheduler handles retain the context's address");

    auto get_scheduler() noexcept -> scheduler { return scheduler{*this}; }

private:
    struct handler {
        gptimer_context* context;
        bool operator()(const gptimer_alarm_event_data_t&) noexcept;
    } handler_{this};
    auto submit(alarm_job& job, std::chrono::nanoseconds delay) noexcept
        -> std::expected<void, error>;
    gptimer timer_;
    esp::freertos::spinlock mutex_;
    alarm_job* pending_ = nullptr;
    std::uint64_t registered_at_ = 0;
    std::uint64_t deadline_ = 0;
};

inline auto schedule_after(gptimer_context::scheduler scheduler,
                           std::chrono::nanoseconds delay) noexcept {
    return scheduler.schedule_after(delay);
}

} // namespace esp::execution::idf
