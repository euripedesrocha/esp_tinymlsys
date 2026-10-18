// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/connect.hpp"
#include "esp_execution/environment.hpp"
#include "esp_execution/operation_state.hpp"

namespace esp_execution_test {

namespace execution = esp::execution;

struct value_only_receiver {
    using receiver_concept = execution::receiver_tag;
    int* completions;

    void set_value() && noexcept { ++*completions; }
};

enum class terminate_as : uint8_t {
    error,
    stopped,
};

struct child_lifetime {
    int constructed = 0;
    int destroyed = 0;
    int live = 0;
    bool replacement_order_valid = true;
};

struct empty_value_completion {
    using signature = execution::set_value_t();

    static void complete(auto&& receiver, int /*unused*/) noexcept {
        execution::set_value(std::forward<decltype(receiver)>(receiver));
    }
};

struct iteration_value_completion {
    using signature = execution::set_value_t(int);

    static void complete(auto&& receiver, int iteration) noexcept {
        execution::set_value(std::forward<decltype(receiver)>(receiver), iteration);
    }
};

template <class ValueCompletion> struct basic_iteration_sender {
    template <class Receiver> class operation {
    public:
        using operation_state_concept = execution::operation_state_tag;

        operation(int& starts, int iterations_before_terminal, terminate_as terminal,
                  Receiver receiver, child_lifetime* lifetime)
            : iterations_started_(&starts), iterations_before_terminal_(iterations_before_terminal),
              terminal_(terminal), receiver_(std::move(receiver)), lifetime_(lifetime) {
            if (lifetime_ != nullptr) {
                lifetime_->replacement_order_valid &= lifetime_->live == 0;
                lifetime_->constructed += 1;
                lifetime_->live += 1;
            }
        }

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        ~operation() {
            if (lifetime_ != nullptr) {
                lifetime_->destroyed += 1;
                lifetime_->live -= 1;
            }
        }

        void start() & noexcept {
            *iterations_started_ += 1;
            if (*iterations_started_ <= iterations_before_terminal_) {
                ValueCompletion::complete(std::move(receiver_), *iterations_started_);
            } else if (terminal_ == terminate_as::error) {
                execution::set_error(std::move(receiver_), *iterations_started_);
            } else {
                execution::set_stopped(std::move(receiver_));
            }
        }

    private:
        int* iterations_started_;
        int iterations_before_terminal_;
        terminate_as terminal_;
        Receiver receiver_;
        child_lifetime* lifetime_;
    };

    using sender_concept = execution::sender_tag;

    template <class Env>
    using completion_signatures =
        execution::completion_signatures<typename ValueCompletion::signature,
                                         execution::set_error_t(int), execution::set_stopped_t()>;

    auto get_env() const noexcept -> execution::empty_env { return {}; }

    auto connect(execution::receiver auto&& receiver) const& {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{*iterations_started, iterations_before_terminal, terminal,
                                        std::forward<decltype(receiver)>(receiver), lifetime};
    }

    int* iterations_started;
    int iterations_before_terminal;
    terminate_as terminal;
    child_lifetime* lifetime = nullptr;
};

using iteration_sender = basic_iteration_sender<empty_value_completion>;
using numbered_iteration_sender = basic_iteration_sender<iteration_value_completion>;

template <class Source, class Receiver>
concept source_connectable_to = requires(Source& source, Receiver&& receiver) {
    { source.connect(std::forward<Receiver>(receiver)) } -> execution::operation_state;
};

template <execution::sender Source> class move_only_sender {
public:
    using sender_concept = execution::sender_tag;

    template <class Environment>
        requires execution::sender_in<Source, Environment>
    using completion_signatures = execution::completion_signatures_of_t<Source, Environment>;

    explicit move_only_sender(Source source)
        : source_(std::make_unique<Source>(std::move(source))) {}

    decltype(auto) get_env() const noexcept { return execution::get_env(*source_); }

    auto connect(execution::receiver auto&& receiver) const& noexcept(
        noexcept(source_->connect(std::forward<decltype(receiver)>(receiver))))
        requires source_connectable_to<Source, decltype(receiver)>
    {
        return source_->connect(std::forward<decltype(receiver)>(receiver));
    }

private:
    std::unique_ptr<Source> source_;
};

struct multi_value_sender {
    template <class Receiver> class operation {
    public:
        using operation_state_concept = execution::operation_state_tag;

        operation(Receiver receiver, int* starts)
            : receiver_(std::move(receiver)), starts_(starts) {}

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            if (starts_ == nullptr) {
                execution::set_value(std::move(receiver_), 1, 2);
                return;
            }
            const auto iteration = ++*starts_;
            if (iteration > 4) {
                execution::set_error(std::move(receiver_), iteration);
            } else if (iteration % 2 == 1) {
                execution::set_value(std::move(receiver_), 1);
            } else {
                execution::set_value(std::move(receiver_), 1, 2);
            }
        }

    private:
        Receiver receiver_;
        int* starts_;
    };

    using sender_concept = execution::sender_tag;

    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(int),
                                         execution::set_value_t(int, int),
                                         execution::set_error_t(int), execution::set_stopped_t()>;

    auto connect(execution::receiver auto&& receiver) const& {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{std::forward<decltype(receiver)>(receiver), starts};
    }

    int* starts = nullptr;
};

// These fixtures are used only to check constraints, not to execute work.
struct probe_operation {
    using operation_state_concept = execution::operation_state_tag;
    void start() & noexcept {}
};

struct rvalue_only_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = execution::completion_signatures<execution::set_value_t()>;
    auto connect(execution::receiver auto&&) && { return probe_operation{}; }
};

struct result {
    int values = 0;
    int value = 0;
    int errors = 0;
    int error = 0;
    int stopped = 0;
};

struct result_receiver {
    using receiver_concept = execution::receiver_tag;

    result* state;

    auto get_env() const noexcept -> execution::empty_env { return {}; }

    void set_value() && noexcept { state->values += 1; }
    void set_value(int value) && noexcept {
        state->values += 1;
        state->value = value;
    }
    void set_error(int error) && noexcept {
        state->errors += 1;
        state->error = error;
    }

    void set_stopped() && noexcept { state->stopped += 1; }
};

struct destruction_record {
    void* operation = nullptr;
    void (*destroy)(void*) noexcept = nullptr;
    result completion;
};

struct destroying_receiver {
    using receiver_concept = execution::receiver_tag;

    explicit destroying_receiver(destruction_record* record) noexcept : state(record) {}

    destruction_record* state;

    auto get_env() const noexcept -> execution::empty_env { return {}; }

    void set_value() && noexcept {
        state->completion.values += 1;
        destroy_operation();
    }

    void set_error(int error) && noexcept {
        state->completion.errors += 1;
        state->completion.error = error;
        destroy_operation();
    }

    void set_stopped() && noexcept {
        state->completion.stopped += 1;
        destroy_operation();
    }

private:
    void destroy_operation() noexcept {
        auto* operation = std::exchange(state->operation, nullptr);
        state->destroy(operation);
    }
};

struct value_destroying_receiver : destroying_receiver {
    using destroying_receiver::destroying_receiver;
    using destroying_receiver::set_value;

    void set_value(int value) && noexcept {
        state->completion.value = value;
        std::move(static_cast<destroying_receiver&>(*this)).set_value();
    }
};

template <class Receiver = destroying_receiver, class Sender, class Run>
result run_and_destroy(Sender sender, Run run) {
    destruction_record state;
    using operation_type = decltype(execution::connect(std::move(sender), Receiver{&state}));
    auto* operation = new operation_type(execution::connect(std::move(sender), Receiver{&state}));
    state.operation = operation;
    state.destroy = [](void* erased) noexcept { delete static_cast<operation_type*>(erased); };

    run(*operation);

    REQUIRE(state.operation == nullptr);
    return state.completion;
}

template <class Receiver = destroying_receiver, class Sender>
result complete_and_destroy(Sender sender) {
    return run_and_destroy<Receiver>(std::move(sender),
                                     [](auto& operation) { execution::start(operation); });
}

} // namespace esp_execution_test
