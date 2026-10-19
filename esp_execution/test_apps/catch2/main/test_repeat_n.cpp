// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/just.hpp"
#include "esp_execution/repeat.hpp"

#include "deferred_sender_test_support.hpp"
#include "repeat_test_support.hpp"
#include "threaded_sender_test_support.hpp"

#include <memory>
#include <catch2/generators/catch_generators.hpp>

namespace {

using namespace esp_execution_test;

template <class Sender>
concept can_repeat_n =
    requires(Sender&& sender) { execution::repeat_n(std::forward<Sender>(sender), 1); };

template <class Sender>
concept can_connect_to_result = requires(Sender&& sender) {
    execution::connect(std::forward<Sender>(sender), result_receiver{});
};

class owning_iteration_sender {
public:
    using sender_concept = execution::sender_tag;

    template <class Env> using completion_signatures = iteration_sender::completion_signatures<Env>;

    explicit owning_iteration_sender(std::shared_ptr<iteration_sender> source)
        : source_(std::move(source)) {}

    auto connect(execution::receiver auto&& receiver) const& {
        return source_->connect(std::forward<decltype(receiver)>(receiver));
    }

private:
    std::shared_ptr<iteration_sender> source_;
};

} // namespace

TEST_CASE("repeat_n exposes value and predecessor terminal completions", "[repeat_n][signatures]") {
    using repeated_sender =
        decltype(execution::repeat_n(std::declval<numbered_iteration_sender>(), 3));
    STATIC_REQUIRE(
        std::same_as<
            execution::completion_signatures_of_t<repeated_sender>,
            execution::completion_signatures<execution::set_value_t(), execution::set_error_t(int),
                                             execution::set_stopped_t()>>);
}

TEST_CASE("repeat_n requires a reusable sender with value completions", "[repeat_n][constraints]") {
    STATIC_REQUIRE(can_repeat_n<decltype(execution::just(1))>);
    STATIC_REQUIRE_FALSE(can_repeat_n<rvalue_only_sender>);
}

TEST_CASE("repeat_n needs only its advertised value handler", "[repeat_n][signatures]") {
    const auto count = GENERATE(0, 1, 3);
    int completions = 0;
    auto work = execution::repeat_n(execution::just(), count);
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t()>>);
    auto operation = execution::connect(work, value_only_receiver{.completions = &completions});
    execution::start(operation);
    REQUIRE(completions == 1);
}

TEST_CASE("repeat_n completes from cross-thread values while its receiver destroys the operation",
          "[repeat_n][thread][lifecycle]") {
    const auto timing =
        GENERATE(completion_timing::before_start_returns, completion_timing::after_start_returns);
    CAPTURE(timing);

    threaded_completion pending{.timing = timing};
    const auto completion = run_and_destroy(
        execution::repeat_n(threaded_sender{.pending = pending}, 3),
        [&](auto& operation) { pending.run(operation, 2, completion_kind::value); });

    REQUIRE(pending.starts == 3);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_n transfers predecessor ownership on rvalue connection",
          "[repeat_n][ownership]") {
    int starts = 0;
    child_lifetime lifetime;
    result completion;
    auto source =
        std::make_shared<iteration_sender>(iteration_sender{.iterations_started = &starts,
                                                            .iterations_before_terminal = 3,
                                                            .terminal = terminate_as::error,
                                                            .lifetime = &lifetime});
    const std::weak_ptr<iteration_sender> owned_source = source;
    auto work = owning_iteration_sender{std::move(source)} | execution::repeat_n(3);

    {
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        REQUIRE(owned_source.use_count() == 1);
        REQUIRE(starts == 0);
        REQUIRE(lifetime.constructed == 0);
        execution::start(operation);

        REQUIRE(starts == 3);
        REQUIRE(completion.values == 1);
        REQUIRE(completion.errors == 0);
        REQUIRE(completion.stopped == 0);
        REQUIRE(lifetime.constructed == 3);
        REQUIRE(lifetime.destroyed == 2);
        REQUIRE(lifetime.live == 1);
        REQUIRE(lifetime.replacement_order_valid);
    }
    REQUIRE(lifetime.destroyed == 3);
    REQUIRE(lifetime.live == 0);
    REQUIRE(owned_source.expired());
}

TEST_CASE("repeat_n completes after the requested successes", "[repeat_n]") {
    const int count = GENERATE(0, 1, 5);
    CAPTURE(count);

    int starts = 0;
    result completion;
    auto work = numbered_iteration_sender{.iterations_started = &starts,
                                          .iterations_before_terminal = count,
                                          .terminal = terminate_as::error} |
                execution::repeat_n(count);
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    execution::start(operation);

    REQUIRE(starts == count);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_n owns a move-only predecessor and reconnects it without copying",
          "[repeat_n][ownership]") {
    const int count = GENERATE(0, 1, 3);
    CAPTURE(count);
    int starts = 0;
    child_lifetime lifetime;
    result completion;
    using predecessor = move_only_sender<iteration_sender>;
    STATIC_REQUIRE_FALSE(can_connect_to_result<predecessor&>);

    auto work =
        execution::repeat_n(predecessor{iteration_sender{.iterations_started = &starts,
                                                         .iterations_before_terminal = count,
                                                         .terminal = terminate_as::error,
                                                         .lifetime = &lifetime}},
                            count);
    STATIC_REQUIRE(can_connect_to_result<decltype(work)>);
    STATIC_REQUIRE_FALSE(can_connect_to_result<decltype(work)&>);
    {
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        REQUIRE(starts == 0);
        REQUIRE(lifetime.constructed == 0);
        execution::start(operation);
        REQUIRE(starts == count);
        REQUIRE(completion.values == 1);
        REQUIRE(completion.errors == 0);
        REQUIRE(completion.stopped == 0);
    }
    REQUIRE(lifetime.constructed == count);
    REQUIRE(lifetime.destroyed == count);
    REQUIRE(lifetime.live == 0);
    REQUIRE(lifetime.replacement_order_valid);
}

TEST_CASE("repeat_n discards any value arguments", "[repeat_n][values]") {
    int starts = 0;
    result completion;
    auto work = multi_value_sender{.starts = &starts} | execution::repeat_n(2);
    auto operation = execution::connect(work, result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == 2);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_n waits for each deferred success before completing", "[repeat_n][deferred]") {
    const int count = GENERATE(1, 3);
    CAPTURE(count);
    deferred_completion pending;
    result completion;
    auto work = execution::repeat_n(move_only_sender{deferred_sender{.pending = &pending}}, count);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    REQUIRE(pending.starts == 0);
    execution::start(operation);
    REQUIRE(pending.starts == 1);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);

    for (int iteration = 1; iteration <= count; ++iteration) {
        CAPTURE(iteration);
        pending.deliver(completion_kind::value);
        REQUIRE(pending.starts == (iteration < count ? iteration + 1 : count));
        REQUIRE(completion.values == (iteration == count ? 1 : 0));
        REQUIRE(completion.errors == 0);
        REQUIRE(completion.stopped == 0);
        REQUIRE((pending.operation != nullptr) == (iteration < count));
        REQUIRE((pending.complete != nullptr) == (iteration < count));
    }
}

TEST_CASE("repeat_n forwards a terminal completion before its count", "[repeat_n][terminal]") {
    terminate_as terminal = terminate_as::error;
    SECTION("error completion") { terminal = terminate_as::error; }
    SECTION("stopped completion") { terminal = terminate_as::stopped; }

    int starts = 0;
    result completion;
    auto work = execution::repeat_n(numbered_iteration_sender{.iterations_started = &starts,
                                                              .iterations_before_terminal = 1,
                                                              .terminal = terminal},
                                    3);
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    execution::start(operation);

    REQUIRE(starts == 2);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (terminal == terminate_as::error ? 1 : 0));
    REQUIRE(completion.stopped == (terminal == terminate_as::stopped ? 1 : 0));
    if (terminal == terminate_as::error) {
        REQUIRE(completion.error == 2);
    }
}

TEST_CASE("repeat_n permits its operation to be destroyed by value completion",
          "[repeat_n][lifecycle]") {
    int starts = 0;
    const auto completion = complete_and_destroy(
        execution::repeat_n(numbered_iteration_sender{.iterations_started = &starts,
                                                      .iterations_before_terminal = 1,
                                                      .terminal = terminate_as::error},
                            1));
    REQUIRE(starts == 1);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat_n permits its operation to be destroyed by the final deferred value",
          "[repeat_n][deferred][lifecycle]") {
    const int count = GENERATE(1, 3);
    CAPTURE(count);
    deferred_completion pending;
    const auto completion = run_and_destroy(
        execution::repeat_n(deferred_sender{.pending = &pending}, count), [&](auto& operation) {
            execution::start(operation);
            for (int iteration = 1; iteration <= count; ++iteration) {
                CAPTURE(iteration);
                REQUIRE(pending.starts == iteration);
                pending.deliver(completion_kind::value);
            }
        });

    REQUIRE(pending.starts == count);
    REQUIRE(pending.operation == nullptr);
    REQUIRE(pending.complete == nullptr);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}
