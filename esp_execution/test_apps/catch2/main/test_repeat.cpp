// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/just.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"

#include "deferred_sender_test_support.hpp"
#include "repeat_test_support.hpp"
#include "threaded_sender_test_support.hpp"

#include <algorithm>
#include <catch2/generators/catch_generators.hpp>

namespace {

using namespace esp_execution_test;

struct iteration_observations {
    int active_starts = 0;
    int peak_starts = 0;
    bool all_children_aligned = true;
};

struct observed_iteration_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = numbered_iteration_sender::completion_signatures<Env>;

    numbered_iteration_sender source;
    iteration_observations* observations;

    template <class Receiver> class alignas(64) operation {
        using child_operation = decltype(execution::connect(
            std::declval<numbered_iteration_sender&>(), std::declval<Receiver>()));

    public:
        using operation_state_concept = execution::operation_state_tag;

        operation(numbered_iteration_sender source, Receiver receiver,
                  iteration_observations& observations)
            : child_(execution::connect(source, std::move(receiver))),
              observations_(&observations) {
            observations.all_children_aligned &=
                reinterpret_cast<std::uintptr_t>(this) % alignof(operation) == 0;
        }

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            // Completion may destroy this child; only the external record is used afterward.
            auto& observations = *observations_;
            observations.active_starts += 1;
            observations.peak_starts =
                std::max(observations.peak_starts, observations.active_starts);
            execution::start(child_);
            observations.active_starts -= 1;
        }

    private:
        child_operation child_;
        iteration_observations* observations_;
    };

    auto connect(execution::receiver auto&& receiver) const& {
        return operation<std::decay_t<decltype(receiver)>>{
            source, std::forward<decltype(receiver)>(receiver), *observations};
    }
};

struct reference_test_env {
    int value;
};

struct reference_test_sender {
    using sender_concept = execution::sender_tag;

    template <class Environment>
    using completion_signatures = execution::completion_signatures<execution::set_value_t()>;

    reference_test_env environment;

    const reference_test_env& get_env() const noexcept { return environment; }

    auto connect(result_receiver&&) & noexcept { return probe_operation{}; }
};

struct potentially_throwing_reference_sender : reference_test_sender {
    auto connect(result_receiver&&) & { return probe_operation{}; }
};

struct invalid_reference_sender : reference_test_sender {
    int connect(result_receiver&&) & noexcept { return 0; }
};

template <class Sender, class Receiver>
concept can_connect_reference = requires(Sender& source, Receiver&& receiver) {
    execution::connect(execution::detail::repeat::sender_reference{source},
                       std::forward<Receiver>(receiver));
};

template <class Error> struct error_signature_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(), execution::set_error_t(Error)>;
    auto connect(execution::receiver auto&&) const& { return probe_operation{}; }
};

using reference_error_sender = error_signature_sender<int&>;

struct throwing_move_error {
    throwing_move_error(throwing_move_error&&) noexcept(false);
};

struct throwing_destructor_error {
    throwing_destructor_error(throwing_destructor_error&&) noexcept;
    ~throwing_destructor_error() noexcept(false);
};

struct throwing_copy_error {
    throwing_copy_error(throwing_copy_error&&) noexcept;
    throwing_copy_error(const throwing_copy_error&) noexcept(false);
};

struct error_lifetime {
    int live = 0;
    int constructed = 0;
    int destroyed = 0;
    int moved = 0;
    bool all_objects_aligned = true;
    bool delivered_aligned = false;
};

struct alignas(32) tracked_error {
    error_lifetime* lifetime;
    int value;

    tracked_error(error_lifetime& state, int code) noexcept : lifetime(&state), value(code) {
        lifetime->live += 1;
        lifetime->constructed += 1;
        lifetime->all_objects_aligned &=
            reinterpret_cast<std::uintptr_t>(this) % alignof(tracked_error) == 0;
    }

    tracked_error(const tracked_error&) = delete;
    tracked_error(tracked_error&& other) noexcept : tracked_error(*other.lifetime, other.value) {
        lifetime->moved += 1;
    }

    ~tracked_error() {
        lifetime->live -= 1;
        lifetime->destroyed += 1;
    }
};

struct tracked_error_sender {
    using sender_concept = execution::sender_tag;

    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(), execution::set_error_t(int),
                                         execution::set_error_t(tracked_error)>;

    error_lifetime* lifetime;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        error_lifetime* lifetime;
        Receiver receiver;

        void start() & noexcept {
            static_assert(execution::detail::member_set_error_invocable<Receiver, tracked_error>);
            static_assert(!execution::detail::member_set_error_invocable<Receiver, tracked_error&>);
            static_assert(!execution::detail::member_set_error_invocable<Receiver, short>);
            tracked_error error{*lifetime, 42};
            execution::set_error(std::move(receiver), std::move(error));
        }
    };

    auto connect(execution::receiver auto&& receiver) const& {
        return operation<std::decay_t<decltype(receiver)>>{
            lifetime, std::forward<decltype(receiver)>(receiver)};
    }
};

struct error_recording_receiver : result_receiver {
    explicit error_recording_receiver(result* state) noexcept : result_receiver{state} {}
    using result_receiver::set_error;

    void set_error(tracked_error&& error) && noexcept {
        error.lifetime->delivered_aligned =
            reinterpret_cast<std::uintptr_t>(&error) % alignof(tracked_error) == 0;
        std::move(*this).set_error(error.value);
    }
};

struct error_destroying_receiver : destroying_receiver {
    using destroying_receiver::destroying_receiver;
    using destroying_receiver::set_error;

    void set_error(tracked_error&& error) && noexcept {
        error.lifetime->delivered_aligned =
            reinterpret_cast<std::uintptr_t>(&error) % alignof(tracked_error) == 0;
        std::move(*this).set_error(error.value);
    }
};

template <class Sender>
concept can_repeat = requires(Sender&& sender) { execution::repeat(std::forward<Sender>(sender)); };

template <class Sender, class Control>
concept can_form_repeat_sender =
    requires { typename execution::detail::repeat::sender<Sender, Control>; };

template <class Storage, class Error>
concept can_store_error =
    requires(Storage& storage, Error&& error) { storage.emplace(std::forward<Error>(error)); };

} // namespace

TEST_CASE("repeat's sender reference preserves signatures environment and noexcept",
          "[repeat][reference]") {
    using reference = execution::detail::repeat::sender_reference<reference_test_sender>;
    using throwing_reference =
        execution::detail::repeat::sender_reference<potentially_throwing_reference_sender>;
    STATIC_REQUIRE(std::copyable<reference>);
    STATIC_REQUIRE(execution::sender<reference&>);
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<reference>,
                                execution::completion_signatures_of_t<reference_test_sender>>);
    STATIC_REQUIRE(std::same_as<execution::env_of_t<reference>, const reference_test_env&>);
    STATIC_REQUIRE(std::constructible_from<reference, reference_test_sender&>);
    STATIC_REQUIRE_FALSE(std::constructible_from<reference, reference_test_sender&&>);
    STATIC_REQUIRE_FALSE(std::constructible_from<
                         execution::detail::repeat::sender_reference<const reference_test_sender>,
                         const reference_test_sender&&>);
    STATIC_REQUIRE(noexcept(execution::connect(std::declval<reference>(), result_receiver{})));
    STATIC_REQUIRE_FALSE(
        noexcept(execution::connect(std::declval<throwing_reference>(), result_receiver{})));

    reference_test_sender source{.environment = {.value = 42}};
    const auto borrowed = reference{source};
    const auto copy = borrowed;
    REQUIRE(std::addressof(execution::get_env(copy)) == std::addressof(source.environment));
    REQUIRE(execution::get_env(copy).value == 42);
    source.environment.value = 7;
    REQUIRE(execution::get_env(copy).value == 7);
}

TEST_CASE("repeat's sender reference requires a valid lvalue member connection",
          "[repeat][reference][constraints]") {
    STATIC_REQUIRE(can_connect_reference<reference_test_sender, result_receiver>);
    STATIC_REQUIRE_FALSE(can_connect_reference<reference_test_sender, result_receiver&>);
    STATIC_REQUIRE_FALSE(can_connect_reference<rvalue_only_sender, result_receiver>);
    STATIC_REQUIRE_FALSE(can_connect_reference<invalid_reference_sender, result_receiver>);
}

TEST_CASE("repeat reconnects a move-only predecessor until its terminal completion",
          "[repeat][ownership]") {
    const auto terminal = GENERATE(terminate_as::error, terminate_as::stopped);
    CAPTURE(terminal);
    int starts = 0;
    child_lifetime lifetime;
    result completion;
    auto work = move_only_sender{iteration_sender{.iterations_started = &starts,
                                                  .iterations_before_terminal = 3,
                                                  .terminal = terminal,
                                                  .lifetime = &lifetime}} |
                execution::repeat();
    STATIC_REQUIRE_FALSE(std::copy_constructible<decltype(work)>);
    {
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        REQUIRE(starts == 0);
        execution::start(operation);
        REQUIRE(starts == 4);
        REQUIRE(completion.values == 0);
        REQUIRE(completion.errors == (terminal == terminate_as::error ? 1 : 0));
        REQUIRE(completion.stopped == (terminal == terminate_as::stopped ? 1 : 0));
        if (terminal == terminate_as::error) {
            REQUIRE(completion.error == 4);
        }
    }
    REQUIRE(lifetime.constructed == 4);
    REQUIRE(lifetime.destroyed == 4);
    REQUIRE(lifetime.live == 0);
    REQUIRE(lifetime.replacement_order_valid);
}

TEST_CASE("repeat exposes only error and stopped completions", "[repeat][signatures]") {
    int starts = 0;
    using repeated_sender =
        decltype(execution::repeat(iteration_sender{.iterations_started = &starts,
                                                    .iterations_before_terminal = 1,
                                                    .terminal = terminate_as::error}));
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<repeated_sender>,
                                execution::completion_signatures<execution::set_error_t(int),
                                                                 execution::set_stopped_t()>>);
}

TEST_CASE("repeat exposes no completions for a value-only predecessor", "[repeat][signatures]") {
    using repeated_sender = decltype(execution::just() | execution::repeat());
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<repeated_sender>,
                                execution::completion_signatures<>>);

    // Compile the full start path without running this intentionally infinite loop.
    [[maybe_unused]] auto instantiate_start = [] {
        struct empty_receiver {
            using receiver_concept = execution::receiver_tag;
        };
        auto work = execution::repeat(execution::just());
        auto operation = execution::connect(work, empty_receiver{});
        execution::start(operation);
    };
}

TEST_CASE("repeat needs only its advertised error handler", "[repeat][signatures]") {
    struct error_only_receiver {
        using receiver_concept = execution::receiver_tag;
        int* error;
        void set_error(int value) && noexcept { *error = value; }
    };

    int starts = 0;
    int error = 0;
    auto work = iteration_sender{.iterations_started = &starts,
                                 .iterations_before_terminal = 2,
                                 .terminal = terminate_as::error} |
                execution::upon_stopped([]() noexcept {}) | execution::repeat();
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_error_t(int)>>);
    auto operation = execution::connect(work, error_only_receiver{.error = &error});
    execution::start(operation);
    REQUIRE(starts == 3);
    REQUIRE(error == 3);
}

TEST_CASE("repeat needs only its advertised stopped handler", "[repeat][signatures]") {
    struct stopped_only_receiver {
        using receiver_concept = execution::receiver_tag;
        int* completions;
        void set_stopped() && noexcept { ++*completions; }
    };

    int starts = 0;
    int completions = 0;
    auto work = iteration_sender{.iterations_started = &starts,
                                 .iterations_before_terminal = 2,
                                 .terminal = terminate_as::stopped} |
                execution::upon_error([](int) noexcept {}) | execution::repeat();
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_stopped_t()>>);
    auto operation = execution::connect(work, stopped_only_receiver{.completions = &completions});
    execution::start(operation);
    REQUIRE(starts == 3);
    REQUIRE(completions == 1);
}

TEST_CASE("repeat serializes cross-thread completions and forwards its terminal result",
          "[repeat][thread][lifecycle]") {
    const auto timing =
        GENERATE(completion_timing::before_start_returns, completion_timing::after_start_returns);
    const auto terminal = GENERATE(completion_kind::error, completion_kind::stopped);
    CAPTURE(timing, terminal);

    threaded_completion pending{.timing = timing};
    const auto completion =
        run_and_destroy(execution::repeat(threaded_sender{.pending = pending}),
                        [&](auto& operation) { pending.run(operation, 3, terminal); });

    REQUIRE(pending.starts == 4);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (terminal == completion_kind::error ? 1 : 0));
    REQUIRE(completion.stopped == (terminal == completion_kind::stopped ? 1 : 0));
    if (terminal == completion_kind::error) {
        REQUIRE(completion.error == 42);
    }
}

TEST_CASE("repeat requires reusable senders with values and storable errors",
          "[repeat][constraints]") {
    STATIC_REQUIRE(can_repeat<decltype(execution::just())>);
    STATIC_REQUIRE(can_repeat<decltype(execution::just(1))>);
    STATIC_REQUIRE(can_repeat<decltype(execution::just(1, 2))>);
    STATIC_REQUIRE_FALSE(can_repeat<decltype(execution::just_error(1))>);
    STATIC_REQUIRE_FALSE(can_repeat<rvalue_only_sender>);
    STATIC_REQUIRE_FALSE(can_repeat<reference_error_sender>);
    STATIC_REQUIRE_FALSE(can_repeat<error_signature_sender<throwing_move_error>>);
    STATIC_REQUIRE_FALSE(can_repeat<error_signature_sender<throwing_destructor_error>>);
    STATIC_REQUIRE(can_repeat<error_signature_sender<tracked_error>>);
}

TEST_CASE("repeat omits error storage when no errors are advertised", "[repeat][error]") {
    using no_error_storage = execution::detail::repeat::pending_error<
        error_recording_receiver, execution::completion_signatures<execution::set_value_t()>>;
    STATIC_REQUIRE(std::is_empty_v<no_error_storage>);
    STATIC_REQUIRE_FALSE(can_store_error<no_error_storage, int>);
}

TEST_CASE("pending error accepts only declared and nonthrowing error arguments",
          "[repeat][error]") {
    using storage = execution::detail::repeat::pending_error<
        error_recording_receiver,
        execution::completion_signatures<execution::set_error_t(int),
                                         execution::set_error_t(tracked_error),
                                         execution::set_error_t(throwing_copy_error)>>;
    STATIC_REQUIRE(can_store_error<storage, int>);
    STATIC_REQUIRE(can_store_error<storage, int&>);
    STATIC_REQUIRE(can_store_error<storage, tracked_error>);
    STATIC_REQUIRE_FALSE(can_store_error<storage, tracked_error&>);
    STATIC_REQUIRE(can_store_error<storage, throwing_copy_error>);
    STATIC_REQUIRE_FALSE(can_store_error<storage, throwing_copy_error&>);
    STATIC_REQUIRE_FALSE(can_store_error<storage, short>);
}

TEST_CASE("repeat establishes source and control requirements as class invariants",
          "[repeat][constraints]") {
    struct error_only_receiver {
        using receiver_concept = execution::receiver_tag;
        void set_error(int) && noexcept {}
    };
    using namespace execution::detail::repeat;
    STATIC_REQUIRE(can_form_repeat_sender<decltype(execution::just()), forever_control>);
    STATIC_REQUIRE(can_form_repeat_sender<decltype(execution::just()), count_control>);
    STATIC_REQUIRE_FALSE(can_form_repeat_sender<rvalue_only_sender, forever_control>);
    STATIC_REQUIRE_FALSE(
        can_form_repeat_sender<decltype(execution::just_error(7)), forever_control>);
    STATIC_REQUIRE_FALSE(can_form_repeat_sender<reference_error_sender, forever_control>);
    STATIC_REQUIRE_FALSE(can_form_repeat_sender<decltype(execution::just()), int>);

    using finite = decltype(execution::repeat_n(execution::just(), 1));
    STATIC_REQUIRE_FALSE(execution::detail::receiver_for_sender<error_only_receiver, finite>);
}

TEST_CASE("pending error destroys its owned error", "[repeat][error][lifecycle]") {
    using storage = execution::detail::repeat::pending_error<
        error_recording_receiver,
        execution::completion_signatures<execution::set_error_t(int),
                                         execution::set_error_t(tracked_error)>>;
    error_lifetime lifetime;
    result completion;
    error_recording_receiver receiver{&completion};
    {
        storage error;
        error.emplace(tracked_error{lifetime, 42});
        REQUIRE(lifetime.live == 1);

        SECTION("replacement destroys the previous error") {
            error.emplace(17);
            REQUIRE(lifetime.live == 0);
            error.complete(receiver);
            REQUIRE(completion.errors == 1);
            REQUIRE(completion.error == 17);
        }

        SECTION("delivery destroys the stored error and its moved result") {
            error.complete(receiver);
            REQUIRE(completion.errors == 1);
            REQUIRE(completion.error == 42);
            REQUIRE(lifetime.live == 0);
            REQUIRE(lifetime.delivered_aligned);
        }

        SECTION("storage can be reused after delivery") {
            error.complete(receiver);
            error.emplace(17);
            error.complete(receiver);
            REQUIRE(completion.errors == 2);
            REQUIRE(completion.error == 17);
            REQUIRE(lifetime.live == 0);
        }

        SECTION("abandonment leaves cleanup to the storage destructor") {
            // Leave the error stored; the common checks below verify destruction.
        }
    }
    REQUIRE(lifetime.live == 0);
    REQUIRE(lifetime.constructed == lifetime.destroyed);
    REQUIRE(lifetime.all_objects_aligned);
}

TEST_CASE("repeat delivers move-only aligned errors while the receiver destroys its parent",
          "[repeat][error][lifecycle]") {
    error_lifetime lifetime;
    const auto completion = complete_and_destroy<error_destroying_receiver>(
        execution::repeat(tracked_error_sender{.lifetime = &lifetime}));
    REQUIRE(completion.errors == 1);
    REQUIRE(completion.error == 42);
    REQUIRE(lifetime.moved == 2);
    REQUIRE(lifetime.live == 0);
    REQUIRE(lifetime.constructed == lifetime.destroyed);
    REQUIRE(lifetime.all_objects_aligned);
    REQUIRE(lifetime.delivered_aligned);
}

TEST_CASE("repeat iterates synchronously without growing the stack", "[repeat][lifecycle]") {
    constexpr int repetitions = 10000;
    int starts = 0;
    iteration_observations observations;
    result completion;
    auto work = observed_iteration_sender{.source = {.iterations_started = &starts,
                                                     .iterations_before_terminal = repetitions,
                                                     .terminal = terminate_as::error},
                                          .observations = &observations} |
                execution::repeat();
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == repetitions + 1);
    REQUIRE(observations.peak_starts == 1);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 1);
    REQUIRE(completion.error == repetitions + 1);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat forwards stopped without another iteration", "[repeat][stopped]") {
    int starts = 0;
    result completion;
    auto work = execution::repeat(iteration_sender{.iterations_started = &starts,
                                                   .iterations_before_terminal = 3,
                                                   .terminal = terminate_as::stopped});
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == 4);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 1);
}

TEST_CASE("repeat aligns each child and destroys it before reconstructing its replacement",
          "[repeat][lifecycle]") {
    child_lifetime lifetime;
    int starts = 0;
    iteration_observations observations;
    result completion;
    {
        auto work =
            execution::repeat(observed_iteration_sender{.source = {.iterations_started = &starts,
                                                                   .iterations_before_terminal = 4,
                                                                   .terminal = terminate_as::error,
                                                                   .lifetime = &lifetime},
                                                        .observations = &observations});
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        REQUIRE(lifetime.constructed == 0);

        execution::start(operation);

        REQUIRE(starts == 5);
        REQUIRE(completion.errors == 1);
        REQUIRE(lifetime.constructed == 5);
        REQUIRE(lifetime.destroyed == 4);
        REQUIRE(lifetime.live == 1);
        REQUIRE(lifetime.replacement_order_valid);
        REQUIRE(observations.all_children_aligned);
    }
    REQUIRE(lifetime.live == 0);
    REQUIRE(lifetime.destroyed == lifetime.constructed);
}

TEST_CASE("repeat discards alternating value completion shapes", "[repeat][values]") {
    int starts = 0;
    result completion;
    auto work = execution::repeat(multi_value_sender{.starts = &starts});
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == 5);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 1);
    REQUIRE(completion.error == 5);
    REQUIRE(completion.stopped == 0);
}

TEST_CASE("repeat resumes after deferred values and forwards deferred terminal completions",
          "[repeat][deferred]") {
    deferred_completion pending;
    result completion;
    auto work = execution::repeat(deferred_sender{.pending = &pending});
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    REQUIRE(pending.starts == 0);
    execution::start(operation);
    REQUIRE(pending.starts == 1);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);

    pending.deliver(completion_kind::value);
    REQUIRE(pending.starts == 2);
    pending.deliver(completion_kind::value);
    REQUIRE(pending.starts == 3);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);

    SECTION("error ends the loop") {
        pending.deliver(completion_kind::error);
        REQUIRE(completion.errors == 1);
        REQUIRE(completion.error == 42);
        REQUIRE(completion.stopped == 0);
    }
    SECTION("stopped ends the loop") {
        pending.deliver(completion_kind::stopped);
        REQUIRE(completion.errors == 0);
        REQUIRE(completion.stopped == 1);
    }
    REQUIRE(completion.values == 0);
    REQUIRE(pending.starts == 3);
}

TEST_CASE("repeat permits its operation to be destroyed by a synchronous terminal completion",
          "[repeat][lifecycle]") {
    int starts = 0;
    child_lifetime lifetime;
    terminate_as terminal = terminate_as::error;
    SECTION("error completion") { terminal = terminate_as::error; }
    SECTION("stopped completion") { terminal = terminate_as::stopped; }

    const auto completion =
        complete_and_destroy(execution::repeat(iteration_sender{.iterations_started = &starts,
                                                                .iterations_before_terminal = 0,
                                                                .terminal = terminal,
                                                                .lifetime = &lifetime}));

    REQUIRE(starts == 1);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (terminal == terminate_as::error ? 1 : 0));
    REQUIRE(completion.stopped == (terminal == terminate_as::stopped ? 1 : 0));
    REQUIRE(lifetime.constructed == 1);
    REQUIRE(lifetime.destroyed == 1);
    REQUIRE(lifetime.live == 0);
}

TEST_CASE("repeat permits its operation to be destroyed by a deferred terminal completion",
          "[repeat][deferred][lifecycle]") {
    deferred_completion pending;
    completion_kind terminal = completion_kind::error;
    SECTION("error completion") { terminal = completion_kind::error; }
    SECTION("stopped completion") { terminal = completion_kind::stopped; }

    const auto completion = run_and_destroy(execution::repeat(deferred_sender{.pending = &pending}),
                                            [&](auto& operation) {
                                                execution::start(operation);
                                                pending.deliver(completion_kind::value);
                                                REQUIRE(pending.starts == 2);
                                                pending.deliver(terminal);
                                            });

    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (terminal == completion_kind::error ? 1 : 0));
    REQUIRE(completion.stopped == (terminal == completion_kind::stopped ? 1 : 0));
    if (terminal == completion_kind::error) {
        REQUIRE(completion.error == 42);
    }
    REQUIRE(pending.starts == 2);
}

TEST_CASE("repeat and upon_error keep error policy outside the loop", "[repeat][upon_error]") {
    int starts = 0;
    int handled = 0;
    result completion;
    auto work = iteration_sender{.iterations_started = &starts,
                                 .iterations_before_terminal = 5,
                                 .terminal = terminate_as::error} |
                execution::repeat() |
                execution::upon_error([&handled](int error) noexcept { handled = error; });
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});

    execution::start(operation);

    REQUIRE(starts == 6);
    REQUIRE(handled == 6);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.errors == 0);
}
