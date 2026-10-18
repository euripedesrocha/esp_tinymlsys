// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <semaphore>
#include <thread>
#include <utility>

#include "deferred_sender_test_support.hpp"

namespace esp_execution_test {

enum class completion_timing { before_start_returns, after_start_returns };

// A single worker delivers completions. Each publication transfers the pending
// child to that worker; no test assertions execute on the worker thread.
struct threaded_completion {
    completion_timing timing;
    std::binary_semaphore ready{0};
    std::binary_semaphore delivered{0};
    std::binary_semaphore start_returned{0};
    int starts = 0;
    void* operation = nullptr;
    void (*complete)(void*, completion_kind) noexcept = nullptr;

    void deliver(completion_kind kind) {
        ready.acquire();
        auto* pending = operation;
        auto callback = complete;
        callback(pending, kind);
        if (timing == completion_timing::before_start_returns) {
            delivered.release();
        }
    }

    void run(auto& operation, int values_before_terminal, completion_kind terminal) {
        std::jthread worker([&] {
            if (timing == completion_timing::after_start_returns) {
                start_returned.acquire();
            }
            for (int iteration = 0; iteration < values_before_terminal; ++iteration) {
                deliver(completion_kind::value);
            }
            deliver(terminal);
        });
        esp::execution::start(operation);
        if (timing == completion_timing::after_start_returns) {
            start_returned.release();
        }
        worker.join();
    }
};

struct threaded_sender {
    using sender_concept = esp::execution::sender_tag;

    template <class Env>
    using completion_signatures =
        esp::execution::completion_signatures<esp::execution::set_value_t(),
                                              esp::execution::set_error_t(int),
                                              esp::execution::set_stopped_t()>;

    threaded_completion& pending;

    template <class Receiver> class operation {
    public:
        using operation_state_concept = esp::execution::operation_state_tag;

        operation(threaded_completion& pending, Receiver receiver)
            : pending_(pending), receiver_(std::move(receiver)) {}

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            // Keep all state needed after publication outside this operation:
            // completion can destroy the child before start returns.
            auto& pending = pending_;
            pending.starts += 1;
            pending.operation = this;
            pending.complete = [](void* erased, completion_kind kind) noexcept {
                auto receiver = std::move(static_cast<operation*>(erased)->receiver_);
                switch (kind) {
                case completion_kind::value:
                    esp::execution::set_value(std::move(receiver));
                    break;
                case completion_kind::error:
                    esp::execution::set_error(std::move(receiver), 42);
                    break;
                case completion_kind::stopped:
                    esp::execution::set_stopped(std::move(receiver));
                    break;
                }
            };
            pending.ready.release();
            if (pending.timing == completion_timing::before_start_returns) {
                pending.delivered.acquire();
            }
        }

    private:
        threaded_completion& pending_;
        Receiver receiver_;
    };

    auto connect(esp::execution::receiver auto&& receiver) const& {
        return operation<std::decay_t<decltype(receiver)>>{
            pending, std::forward<decltype(receiver)>(receiver)};
    }
};

} // namespace esp_execution_test
