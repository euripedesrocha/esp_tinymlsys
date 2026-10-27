// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <type_traits>
#include <utility>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_freertos_cpp/chrono.hpp"
#include "esp_execution/scheduler.hpp"

namespace esp::execution::freertos {

enum class schedule_error : std::uint8_t {
    queue_full,
    queue_timeout,
};

/** Queue admission only: waiting does not impose a deadline on accepted work. */
class submission_policy {
public:
    [[nodiscard]] static constexpr auto fail_if_full() noexcept -> submission_policy { return {0}; }
    [[nodiscard]] static constexpr auto wait() noexcept -> submission_policy {
        return {portMAX_DELAY};
    }
    /** Positive finite timeout, rounded up to the next whole FreeRTOS tick. */
    [[nodiscard]] static constexpr auto wait_for(std::chrono::nanoseconds timeout) noexcept
        -> submission_policy pre(timeout > std::chrono::nanoseconds::zero()) {
        return {esp::freertos::to_ticks(timeout)};
    }

private:
    friend class task_context;
    constexpr submission_policy(TickType_t ticks) noexcept : wait_ticks_(ticks) {}
    TickType_t wait_ticks_;
};

namespace detail {

struct task_job {
    void* state = nullptr;
    void (*complete)(void*) noexcept = nullptr;
};

enum class submission_result : std::uint8_t {
    accepted,
    queue_full,
    queue_timeout,
    stopped,
};

} // namespace detail

/**
 * A persistent FreeRTOS task and bounded mailbox that form one execution resource.
 *
 * Scheduler handles and started operations are non-owning. They must not outlive
 * their context. Successful submissions complete on the worker; rejection
 * completes inline on the submitter, including ISR context. ISR callers and
 * their failure receivers must use only ISR-safe, nonblocking operations.
 * Waiting admission policies block the submitting task in start(), not the
 * worker. Starting a waiting submission from an ISR or the worker itself is a
 * contract violation, even if space is available or the context is finished.
 *
 * Stop all producers and wait for any in-progress submissions to return before
 * calling finish(). finish() and destruction are owner-task
 * lifecycle operations: neither may run on the worker or in an ISR, and neither
 * may race submission. Accepted work drains before the worker exits.
 */
class task_context {
public:
    struct config {
        /** Optional diagnostic name, passed directly to FreeRTOS. */
        const char* name = nullptr;
        std::uint32_t stack_size = 4096;
        UBaseType_t priority = tskIDLE_PRIORITY + 1;
        BaseType_t core_id = tskNO_AFFINITY;
        UBaseType_t queue_capacity = 8;
    };

    class scheduler {
    public:
        class schedule_sender {
            template <class Receiver> class operation {
            public:
                using operation_state_concept = operation_state_tag;

                operation(task_context& context, submission_policy policy, Receiver receiver)
                    : context_{&context}, policy_{policy}, receiver_{std::move(receiver)} {}

                operation(const operation&) =
                    delete ("a queued operation uniquely owns its receiver");
                operation(operation&&) =
                    delete ("the task queue retains the operation state's address");
                operation& operator=(const operation&) =
                    delete ("a queued operation uniquely owns its receiver");
                operation& operator=(operation&&) =
                    delete ("the task queue retains the operation state's address");

                void start() & noexcept {
                    switch (context_->submit({this, complete_value}, policy_)) {
                    case detail::submission_result::accepted:
                        return;
                    case detail::submission_result::queue_full:
                        execution::set_error(std::move(receiver_), schedule_error::queue_full);
                        return;
                    case detail::submission_result::queue_timeout:
                        execution::set_error(std::move(receiver_), schedule_error::queue_timeout);
                        return;
                    case detail::submission_result::stopped:
                        execution::set_stopped(std::move(receiver_));
                        return;
                    }
                }

            private:
                static void complete_value(void* state) noexcept {
                    auto& self = *static_cast<operation*>(state);
                    execution::set_value(std::move(self.receiver_));
                }

                task_context* context_;
                submission_policy policy_;
                Receiver receiver_;
            };

        public:
            using sender_concept = sender_tag;

            struct environment {
                task_context* context;
                submission_policy policy;

                [[nodiscard]] auto query(get_completion_scheduler_t<set_value_t>) const noexcept
                    -> scheduler {
                    return scheduler{*context, policy};
                }
            };

            template <class Env>
            using completion_signatures =
                execution::completion_signatures<execution::set_value_t(),
                                                 execution::set_error_t(schedule_error),
                                                 execution::set_stopped_t()>;

            explicit schedule_sender(task_context& context, submission_policy policy) noexcept
                : context_{&context}, policy_{policy} {}

            [[nodiscard]] auto get_env() const noexcept -> environment {
                return {context_, policy_};
            }

            [[nodiscard]] auto
            connect(execution::detail::receiver_for_sender<schedule_sender> auto&& receiver) const {
                using receiver_type = std::decay_t<decltype(receiver)>;
                return operation<receiver_type>{*context_, policy_,
                                                std::forward<decltype(receiver)>(receiver)};
            }

        private:
            task_context* context_;
            submission_policy policy_;
        };

        using scheduler_concept = scheduler_tag;

        [[nodiscard]] auto schedule() const noexcept -> schedule_sender {
            return schedule_sender{*context_, policy_};
        }

        // Admission choices do not change the associated execution resource.
        friend constexpr bool operator==(const scheduler& left, const scheduler& right) noexcept {
            return left.context_ == right.context_;
        }

    private:
        friend class task_context;

        explicit scheduler(task_context& context, submission_policy policy) noexcept
            : context_{&context}, policy_{policy} {}

        task_context* context_;
        submission_policy policy_;
    };

    /** Eagerly creates the queue and persistent worker task. */
    explicit task_context(config configuration) pre(configuration.stack_size > 0)
        pre(configuration.queue_capacity > 0);
    ~task_context();

    task_context(const task_context&) = delete ("a context uniquely owns its task and mailbox");
    task_context(task_context&&) = delete ("scheduler handles retain the context's stable address");
    task_context&
    operator=(const task_context&) = delete ("a context uniquely owns its task and mailbox");
    task_context&
    operator=(task_context&&) = delete ("scheduler handles retain the context's stable address");

    [[nodiscard]] auto
    get_scheduler(submission_policy policy = submission_policy::fail_if_full()) noexcept
        -> scheduler {
        return scheduler{*this, policy};
    }

    /** Stops accepting work; may block until the queue has room for the stop marker. */
    void finish() noexcept;

private:
    [[nodiscard]] auto submit(detail::task_job job, submission_policy policy) noexcept
        -> detail::submission_result;
    void run() noexcept;
    static void task_entry(void* state) noexcept;

    QueueHandle_t queue_ = nullptr;
    SemaphoreHandle_t completed_ = nullptr;
    TaskHandle_t task_ = nullptr;
    std::atomic_bool accepting_ = true;
};

} // namespace esp::execution::freertos
