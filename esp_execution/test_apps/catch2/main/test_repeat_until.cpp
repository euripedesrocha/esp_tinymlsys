// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/just.hpp"
#include "esp_execution/repeat.hpp"

#include "deferred_sender_test_support.hpp"
#include "repeat_test_support.hpp"

#include <memory>
#include <tuple>
#include <catch2/generators/catch_generators.hpp>

namespace {

using namespace esp_execution_test;

struct repeat_test_env {};

struct environment_dependent_sender {
    template <class Receiver> class operation {
    public:
        using operation_state_concept = execution::operation_state_tag;

        explicit operation(Receiver receiver) : receiver_(std::move(receiver)) {}

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            if constexpr (std::same_as<execution::env_of_t<Receiver>, repeat_test_env>) {
                execution::set_value(std::move(receiver_), 1, 2);
            } else {
                execution::set_value(std::move(receiver_), 1);
            }
        }

    private:
        Receiver receiver_;
    };

    using sender_concept = execution::sender_tag;

    template <class Env>
    using completion_signatures =
        std::conditional_t<std::same_as<Env, repeat_test_env>,
                           execution::completion_signatures<execution::set_value_t(int, int)>,
                           execution::completion_signatures<execution::set_value_t(int)>>;

    auto connect(execution::receiver auto&& receiver) const& {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{std::forward<decltype(receiver)>(receiver)};
    }
};

struct repeat_test_env_receiver : result_receiver {
    auto get_env() const noexcept -> repeat_test_env { return {}; }
};

struct multi_value_predicate {
    bool operator()(int) noexcept { return false; }
    bool operator()(int, int) noexcept { return true; }
};

struct single_value_predicate {
    bool operator()(int) noexcept { return false; }
};

struct throwing_predicate {
    bool operator()(int) { return false; }
};

template <class Sender, class Predicate>
concept can_repeat_until = requires(Sender&& sender, Predicate&& predicate) {
    execution::repeat_until(std::forward<Sender>(sender), std::forward<Predicate>(predicate));
};

template <class Sender, class Receiver>
concept can_connect = requires(Sender&& sender, Receiver&& receiver) {
    execution::connect(std::forward<Sender>(sender), std::forward<Receiver>(receiver));
};

} // namespace

TEST_CASE("repeat_until exposes value and predecessor terminal completions",
          "[repeat_until][signatures]") {
    using repeated_sender =
        decltype(execution::repeat_until(multi_value_sender{}, multi_value_predicate{}));
    STATIC_REQUIRE(
        std::same_as<
            execution::completion_signatures_of_t<repeated_sender>,
            execution::completion_signatures<execution::set_value_t(), execution::set_error_t(int),
                                             execution::set_stopped_t()>>);
}

TEST_CASE("repeat_until requires a nonthrowing predicate for every value shape",
          "[repeat_until][constraints]") {
    STATIC_REQUIRE(can_repeat_until<multi_value_sender, multi_value_predicate>);
    STATIC_REQUIRE_FALSE(can_repeat_until<multi_value_sender, single_value_predicate>);
    STATIC_REQUIRE_FALSE(can_repeat_until<decltype(execution::just(1)), throwing_predicate>);
    auto non_boolean_predicate = [](int) noexcept {};
    STATIC_REQUIRE_FALSE(
        can_repeat_until<decltype(execution::just(1)), decltype(non_boolean_predicate)>);
}

TEST_CASE("repeat_until needs only its advertised value handler", "[repeat_until][signatures]") {
    int completions = 0;
    int iterations = 0;
    auto work = execution::repeat_until(execution::just(),
                                        [&iterations]() noexcept { return ++iterations == 3; });
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t()>>);
    auto operation = execution::connect(work, value_only_receiver{.completions = &completions});
    execution::start(operation);
    REQUIRE(iterations == 3);
    REQUIRE(completions == 1);
}

TEST_CASE("repeat_until validates its predicate against the receiver environment",
          "[repeat_until][environment]") {
    using default_only =
        decltype(execution::repeat_until(environment_dependent_sender{}, single_value_predicate{}));
    using environment_aware =
        decltype(execution::repeat_until(environment_dependent_sender{}, multi_value_predicate{}));

    STATIC_REQUIRE_FALSE(can_connect<default_only, repeat_test_env_receiver>);
    STATIC_REQUIRE(can_connect<environment_aware, repeat_test_env_receiver>);

    result completion;
    auto work = execution::repeat_until(move_only_sender{environment_dependent_sender{}},
                                        multi_value_predicate{});
    auto operation =
        execution::connect(std::move(work), repeat_test_env_receiver{{.state = &completion}});
    execution::start(operation);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_until completes when its predicate becomes true", "[repeat_until]") {
    const int successes = GENERATE(1, 4);
    CAPTURE(successes);

    int starts = 0;
    int predicate_calls = 0;
    result completion;
    auto work = numbered_iteration_sender{.iterations_started = &starts,
                                          .iterations_before_terminal = successes,
                                          .terminal = terminate_as::error} |
                execution::repeat_until([&](int value) noexcept {
                    predicate_calls += 1;
                    return value == successes;
                });
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    execution::start(operation);

    REQUIRE(starts == successes);
    REQUIRE(predicate_calls == successes);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_until reconnects a move-only predecessor until its predicate succeeds",
          "[repeat_until][ownership]") {
    int starts = 0;
    int predicate_calls = 0;
    result completion;
    auto work = move_only_sender{numbered_iteration_sender{.iterations_started = &starts,
                                                           .iterations_before_terminal = 3,
                                                           .terminal = terminate_as::error}} |
                execution::repeat_until([&predicate_calls](int iteration) noexcept {
                    predicate_calls += 1;
                    return iteration == 3;
                });
    STATIC_REQUIRE_FALSE(std::copy_constructible<decltype(work)>);
    STATIC_REQUIRE(can_connect<decltype(work), result_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<decltype(work)&, result_receiver>);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    REQUIRE(starts == 0);
    execution::start(operation);
    REQUIRE(starts == 3);
    REQUIRE(predicate_calls == 3);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_until forwards terminal completions", "[repeat_until][terminal]") {
    const auto terminal = GENERATE(terminate_as::error, terminate_as::stopped);
    const int successes = GENERATE(0, 2);
    CAPTURE(terminal, successes);

    int starts = 0;
    int predicate_calls = 0;
    result completion;
    auto work = numbered_iteration_sender{.iterations_started = &starts,
                                          .iterations_before_terminal = successes,
                                          .terminal = terminal} |
                execution::repeat_until([&predicate_calls](int) noexcept {
                    predicate_calls += 1;
                    return false;
                });
    auto operation = execution::connect(work, result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == successes + 1);
    REQUIRE(predicate_calls == successes);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (terminal == terminate_as::error ? 1 : 0));
    REQUIRE(completion.stopped == (terminal == terminate_as::stopped ? 1 : 0));
    if (terminal == terminate_as::error) {
        REQUIRE(completion.error == successes + 1);
    }
}

TEST_CASE("repeat_until invokes the predicate for alternating value shapes",
          "[repeat_until][values]") {
    int starts = 0;
    int single_calls = 0;
    int pair_calls = 0;
    bool arguments_valid = true;
    result completion;
    auto work = multi_value_sender{.starts = &starts} |
                execution::repeat_until([&](auto... values) noexcept {
                    if constexpr (sizeof...(values) == 1) {
                        single_calls += 1;
                        arguments_valid &= std::tuple{values...} == std::tuple{1};
                    } else {
                        pair_calls += 1;
                        arguments_valid &= std::tuple{values...} == std::tuple{1, 2};
                    }
                    return single_calls + pair_calls == 4;
                });
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    execution::start(operation);

    REQUIRE(starts == 4);
    REQUIRE(single_calls == 2);
    REQUIRE(pair_calls == 2);
    REQUIRE(arguments_valid);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_until gives each connection independent predicate state",
          "[repeat_until][ownership]") {
    int starts = 0;
    const auto work =
        iteration_sender{.iterations_started = &starts,
                         .iterations_before_terminal = 6,
                         .terminal = terminate_as::error} |
        execution::repeat_until([remaining = 3]() mutable noexcept { return --remaining == 0; });
    result first_completion;
    result second_completion;
    auto first_operation = execution::connect(work, result_receiver{.state = &first_completion});
    REQUIRE(starts == 0);
    execution::start(first_operation);
    REQUIRE(starts == 3);
    REQUIRE(first_completion.values == 1);
    REQUIRE(first_completion.errors == 0);
    REQUIRE(first_completion.stopped == 0);

    auto second_operation = execution::connect(work, result_receiver{.state = &second_completion});
    REQUIRE(starts == 3);
    execution::start(second_operation);
    REQUIRE(starts == 6);
    REQUIRE(second_completion.values == 1);
    REQUIRE(second_completion.errors == 0);
    REQUIRE(second_completion.stopped == 0);
    REQUIRE(first_completion.values == 1);
}

TEST_CASE("repeat_until owns a move-only predicate through an rvalue adaptor",
          "[repeat_until][ownership]") {
    auto predicate = [remaining = std::make_unique<int>(3)]() mutable noexcept {
        return --*remaining == 0;
    };
    STATIC_REQUIRE(can_repeat_until<iteration_sender, decltype(predicate)>);
    STATIC_REQUIRE_FALSE(can_repeat_until<iteration_sender, decltype(predicate)&>);
    auto adaptor = execution::repeat_until(std::move(predicate));
    STATIC_REQUIRE(std::invocable<decltype(adaptor), iteration_sender>);
    STATIC_REQUIRE_FALSE(std::invocable<decltype(adaptor)&, iteration_sender>);

    int starts = 0;
    result completion;
    auto work = iteration_sender{.iterations_started = &starts,
                                 .iterations_before_terminal = 3,
                                 .terminal = terminate_as::error} |
                std::move(adaptor);
    STATIC_REQUIRE_FALSE(std::copy_constructible<decltype(work)>);
    STATIC_REQUIRE(can_connect<decltype(work), result_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<decltype(work)&, result_receiver>);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    REQUIRE(starts == 0);
    execution::start(operation);
    REQUIRE(starts == 3);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_until waits for deferred values before invoking its predicate",
          "[repeat_until][deferred]") {
    const int successes = GENERATE(1, 3);
    CAPTURE(successes);
    deferred_completion pending;
    int predicate_calls = 0;
    result completion;
    auto work = execution::repeat_until(deferred_sender{.pending = &pending},
                                        [&]() noexcept { return ++predicate_calls == successes; });
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    REQUIRE(pending.starts == 0);
    execution::start(operation);
    REQUIRE(pending.starts == 1);
    REQUIRE(predicate_calls == 0);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);

    for (int iteration = 1; iteration <= successes; ++iteration) {
        CAPTURE(iteration);
        pending.deliver(completion_kind::value);
        REQUIRE(predicate_calls == iteration);
        REQUIRE(pending.starts == (iteration < successes ? iteration + 1 : successes));
        REQUIRE(completion.values == (iteration == successes ? 1 : 0));
        REQUIRE(completion.errors == 0);
        REQUIRE(completion.stopped == 0);
        REQUIRE((pending.operation != nullptr) == (iteration < successes));
        REQUIRE((pending.complete != nullptr) == (iteration < successes));
    }
}

TEST_CASE("repeat_until permits its operation to be destroyed by the final deferred value",
          "[repeat_until][deferred][lifecycle]") {
    const int successes = GENERATE(1, 3);
    CAPTURE(successes);
    deferred_completion pending;
    int predicate_calls = 0;
    const auto completion = run_and_destroy(
        execution::repeat_until(deferred_sender{.pending = &pending},
                                [&]() noexcept { return ++predicate_calls == successes; }),
        [&](auto& operation) {
            execution::start(operation);
            REQUIRE(predicate_calls == 0);
            for (int iteration = 1; iteration <= successes; ++iteration) {
                CAPTURE(iteration);
                REQUIRE(pending.starts == iteration);
                pending.deliver(completion_kind::value);
                REQUIRE(predicate_calls == iteration);
            }
        });

    REQUIRE(predicate_calls == successes);
    REQUIRE(pending.starts == successes);
    REQUIRE(pending.operation == nullptr);
    REQUIRE(pending.complete == nullptr);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}
