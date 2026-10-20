// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/continues_on.hpp"
#include "esp_execution/just.hpp"
#include "manual_scheduler_test_support.hpp"
#include "repeat_test_support.hpp"

#include <catch2/generators/catch_generators.hpp>

namespace {
using namespace esp_execution_test;

struct owning_receiver {
    using receiver_concept = execution::receiver_tag;
    int* result;
    void set_value(std::unique_ptr<int> value) && noexcept { *result = *value; }
    void set_error(int value) && noexcept { *result = -value; }
    void set_stopped() && noexcept { *result = -1; }
};

struct throwing_move {
    throwing_move(const throwing_move&) noexcept;
    throwing_move(throwing_move&&) noexcept(false);
};

struct throwing_copy {
    throwing_copy(const throwing_copy&) noexcept(false);
    throwing_copy(throwing_copy&&) noexcept;
};

template <class... Signatures> struct signature_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = execution::completion_signatures<Signatures...>;
};

struct alternate_env {};
struct unavailable_env {};
struct unstorable_env {};

struct environment_storage_source {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = std::conditional_t<
        std::same_as<Env, unstorable_env>,
        execution::completion_signatures<execution::set_value_t(const throwing_copy&)>,
        execution::completion_signatures<execution::set_value_t(int)>>;
};

struct rvalue_scheduler {
    using scheduler_concept = execution::scheduler_tag;

    struct schedule_sender : decltype(execution::just()) {
        struct environment {
            auto query(execution::get_completion_scheduler_t<execution::set_value_t>) const noexcept
                -> rvalue_scheduler {
                return {};
            }
        };

        auto get_env() const noexcept -> environment { return {}; }
    };

    auto schedule() && noexcept { return schedule_sender{execution::just()}; }
    friend bool operator==(rvalue_scheduler, rvalue_scheduler) noexcept = default;
};

struct payload_destroying_receiver : destroying_receiver {
    using destroying_receiver::destroying_receiver;

    void set_value(std::unique_ptr<int>&& value) && noexcept {
        auto* record = state;
        std::move(static_cast<destroying_receiver&>(*this)).set_value();
        // The payload must remain alive after the parent and its variant die.
        record->completion.value = *value;
    }
};

enum class input_kind { value, const_reference };

struct reference_source {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(int),
                                         execution::set_value_t(const int&)>;

    const int* value;
    input_kind kind;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        const int* value;
        input_kind kind;
        Receiver receiver;

        void start() & noexcept {
            if (kind == input_kind::value) {
                execution::set_value(std::move(receiver), int{*value});
            } else {
                execution::set_value(std::move(receiver), *value);
            }
        }
    };

    auto connect(execution::receiver auto receiver) const {
        return operation<decltype(receiver)>{
            .value = value, .kind = kind, .receiver = std::move(receiver)};
    }
};

struct environment_scheduler {
    using scheduler_concept = execution::scheduler_tag;

    struct schedule_sender {
        using sender_concept = execution::sender_tag;

        struct environment {
            auto query(execution::get_completion_scheduler_t<execution::set_value_t>) const noexcept
                -> environment_scheduler {
                return {};
            }
        };

        auto get_env() const noexcept -> environment { return {}; }

        template <class Env>
            requires(!std::same_as<Env, unavailable_env>)
        using completion_signatures =
            std::conditional_t<std::same_as<Env, alternate_env>,
                               execution::completion_signatures<execution::set_value_t(),
                                                                execution::set_error_t(long)>,
                               execution::completion_signatures<execution::set_value_t(),
                                                                execution::set_error_t(int)>>;

        auto connect(execution::receiver auto receiver) const {
            if constexpr (std::same_as<execution::env_of_t<decltype(receiver)>, alternate_env>) {
                return execution::connect(execution::just_error(73L), std::move(receiver));
            } else {
                return execution::connect(execution::just_error(-1), std::move(receiver));
            }
        }
    };

    auto schedule() const noexcept { return schedule_sender{}; }
    friend bool operator==(environment_scheduler, environment_scheduler) noexcept = default;
};

struct environment_source {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures =
        std::conditional_t<std::same_as<Env, alternate_env>,
                           execution::completion_signatures<execution::set_value_t(int, int)>,
                           execution::completion_signatures<execution::set_value_t(int)>>;
    auto connect(execution::receiver auto receiver) const {
        if constexpr (std::same_as<execution::env_of_t<decltype(receiver)>, alternate_env>) {
            return execution::connect(execution::just(20, 22), std::move(receiver));
        } else {
            return execution::connect(execution::just(42), std::move(receiver));
        }
    }
};

struct pair_receiver {
    using receiver_concept = execution::receiver_tag;
    int* result;
    alternate_env get_env() const noexcept { return {}; }
    void set_value(int first, int second) && noexcept { *result = first + second; }
    void set_error(int error) && noexcept { *result = -error; }
    void set_stopped() && noexcept { *result = -2; }
};

template <class Sender>
concept transferable = requires(Sender source, manual_scheduler scheduler) {
    execution::continues_on(std::move(source), scheduler);
};

template <class Source, class Scheduler>
concept can_form_transfer_sender =
    requires { typename execution::detail::continues_on::sender<Source, Scheduler>; };

template <class Sender, class Receiver>
concept can_connect = requires(Sender&& source, Receiver&& receiver) {
    execution::connect(std::forward<Sender>(source), std::forward<Receiver>(receiver));
};
} // namespace

TEST_CASE("continues_on establishes transfer requirements as a sender class invariant",
          "[continues_on][constraints]") {
    struct value_only_receiver {
        using receiver_concept = execution::receiver_tag;
        void set_value(int) && noexcept {}
    };
    using source = decltype(execution::just(42));
    using work = decltype(execution::continues_on(std::declval<source>(), manual_scheduler{}));
    STATIC_REQUIRE(can_form_transfer_sender<source, manual_scheduler>);
    STATIC_REQUIRE_FALSE(can_form_transfer_sender<int, manual_scheduler>);
    STATIC_REQUIRE_FALSE(can_form_transfer_sender<source, int>);
    STATIC_REQUIRE_FALSE(
        can_form_transfer_sender<decltype(execution::just(std::declval<throwing_move>())),
                                 manual_scheduler>);
    STATIC_REQUIRE(can_form_transfer_sender<signature_sender<execution::set_value_t(throwing_copy)>,
                                            manual_scheduler>);
    STATIC_REQUIRE_FALSE(
        can_form_transfer_sender<signature_sender<execution::set_value_t(const throwing_copy&)>,
                                 manual_scheduler>);
    STATIC_REQUIRE_FALSE(
        can_form_transfer_sender<
            signature_sender<execution::set_value_t(int, const throwing_copy&)>, manual_scheduler>);
    STATIC_REQUIRE_FALSE(
        can_form_transfer_sender<signature_sender<execution::set_error_t(const throwing_copy&)>,
                                 manual_scheduler>);
    STATIC_REQUIRE_FALSE(
        can_form_transfer_sender<signature_sender<execution::set_value_t(const throwing_move&)>,
                                 manual_scheduler>);
    STATIC_REQUIRE(can_connect<work, result_receiver&>);
    STATIC_REQUIRE(can_connect<const work&, result_receiver&>);
    STATIC_REQUIRE_FALSE(can_connect<work, value_only_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<const work&, value_only_receiver>);
}

TEST_CASE("continues_on defers each source completion until scheduling succeeds",
          "[continues_on]") {
    const auto kind =
        GENERATE(completion_kind::value, completion_kind::error, completion_kind::stopped);
    deferred_completion input;
    deferred_completion destination;
    result completion;
    auto work = deferred_sender{.pending = &input} |
                execution::continues_on(manual_scheduler{&destination});
    auto operation = execution::connect(work, result_receiver{.state = &completion});
    REQUIRE(input.starts == 0);
    REQUIRE(destination.starts == 0);
    execution::start(operation);
    REQUIRE(input.starts == 1);
    REQUIRE(destination.starts == 0);
    input.deliver(kind);
    REQUIRE(destination.starts == 1);
    REQUIRE(completion.values + completion.errors + completion.stopped == 0);
    destination.deliver(completion_kind::value);
    REQUIRE(completion.values == (kind == completion_kind::value ? 1 : 0));
    REQUIRE(completion.errors == (kind == completion_kind::error ? 1 : 0));
    REQUIRE(completion.stopped == (kind == completion_kind::stopped ? 1 : 0));
    if (kind == completion_kind::error)
        REQUIRE(completion.error == 42);
}

TEST_CASE("continues_on identifies only its successful destination resource",
          "[continues_on][scheduler][attributes]") {
    deferred_completion destination;
    const manual_scheduler scheduler{&destination};
    const auto work = execution::continues_on(execution::just(42), scheduler);
    const auto attributes = execution::get_env(work);

    REQUIRE(execution::get_completion_scheduler<execution::set_value_t>(attributes) == scheduler);
    STATIC_REQUIRE_FALSE(
        std::invocable<decltype(execution::get_completion_scheduler<execution::set_error_t>),
                       const decltype(attributes)&>);
    STATIC_REQUIRE_FALSE(
        std::invocable<decltype(execution::get_completion_scheduler<execution::set_stopped_t>),
                       const decltype(attributes)&>);
}

TEST_CASE("continues_on schedules its owned scheduler with the declared value category",
          "[continues_on][scheduler]") {
    result completion;
    auto work = execution::continues_on(execution::just(42), rvalue_scheduler{});
    SECTION("rvalue connection") {
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        execution::start(operation);
    }
    SECTION("const lvalue connection") {
        auto operation =
            execution::connect(std::as_const(work), result_receiver{.state = &completion});
        execution::start(operation);
    }
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
}

TEST_CASE("continues_on delivers scheduler rejection instead of the source result",
          "[continues_on][error]") {
    const auto rejection = GENERATE(completion_kind::error, completion_kind::stopped);
    deferred_completion destination;
    result completion;
    auto work = execution::continues_on(execution::just(7), manual_scheduler{&destination});
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    execution::start(operation);
    destination.deliver(rejection);
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == (rejection == completion_kind::error ? 1 : 0));
    REQUIRE(completion.stopped == (rejection == completion_kind::stopped ? 1 : 0));
    if (rejection == completion_kind::error) {
        REQUIRE(completion.error == 42);
    }
}

TEST_CASE("continues_on owns move-only payloads across the handoff", "[continues_on][ownership]") {
    deferred_completion destination;
    int completion = 0;
    auto work = execution::just(std::make_unique<int>(42)) |
                execution::continues_on(manual_scheduler{&destination});
    auto operation = execution::connect(std::move(work), owning_receiver{&completion});
    execution::start(operation);
    REQUIRE(completion == 0);
    destination.deliver(completion_kind::value);
    REQUIRE(completion == 42);
}

TEST_CASE("continues_on detaches borrowed payloads before receiver-triggered destruction",
          "[continues_on][ownership][lifecycle]") {
    SECTION("deferred scheduler") {
        deferred_completion destination;
        auto work = execution::just(std::make_unique<int>(42)) |
                    execution::continues_on(manual_scheduler{&destination});
        const auto completion =
            run_and_destroy<payload_destroying_receiver>(std::move(work), [&](auto& operation) {
                execution::start(operation);
                destination.deliver(completion_kind::value);
            });
        REQUIRE(completion.values == 1);
        REQUIRE(completion.value == 42);
    }
    SECTION("immediate scheduler") {
        const auto completion = complete_and_destroy<payload_destroying_receiver>(
            execution::just(std::make_unique<int>(42)) |
            execution::continues_on(rvalue_scheduler{}));
        REQUIRE(completion.values == 1);
        REQUIRE(completion.value == 42);
    }
}

TEST_CASE("continues_on shares same-decay storage and copies referenced input before handoff",
          "[continues_on][ownership][signatures]") {
    const auto kind = GENERATE(input_kind::value, input_kind::const_reference);
    int original = 42;
    deferred_completion destination;
    result completion;
    auto work = reference_source{.value = &original, .kind = kind} |
                execution::continues_on(manual_scheduler{&destination});
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t(int),
                                                                 execution::set_error_t(int),
                                                                 execution::set_stopped_t()>>);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    execution::start(operation);
    REQUIRE(destination.starts == 1);
    REQUIRE(completion.values == 0);
    original = 99;
    destination.deliver(completion_kind::value);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
}

TEST_CASE("continues_on exposes stored source and scheduler terminal signatures",
          "[continues_on][signatures]") {
    using work = decltype(execution::continues_on(execution::just(42), manual_scheduler{}));
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<work>,
                                execution::completion_signatures<execution::set_value_t(int),
                                                                 execution::set_error_t(int),
                                                                 execution::set_stopped_t()>>);
    STATIC_REQUIRE_FALSE(
        transferable<decltype(execution::just(throwing_move{std::declval<throwing_move>()}))>);
}

TEST_CASE("continues_on uses the receiver environment for source signatures and connection",
          "[continues_on][environment]") {
    deferred_completion destination;
    int completion = 0;
    auto work = environment_source{} | execution::continues_on(manual_scheduler{&destination});
    STATIC_REQUIRE(
        std::same_as<execution::completion_signatures_of_t<decltype(work), alternate_env>,
                     execution::completion_signatures<execution::set_value_t(int, int),
                                                      execution::set_error_t(int),
                                                      execution::set_stopped_t()>>);
    auto operation = execution::connect(work, pair_receiver{&completion});
    execution::start(operation);
    REQUIRE(completion == 0);
    destination.deliver(completion_kind::value);
    REQUIRE(completion == 42);
}

TEST_CASE("continues_on forwards receiver environment to scheduling and its terminal signatures",
          "[continues_on][environment]") {
    auto work = execution::continues_on(environment_source{}, environment_scheduler{});
    using transfer = decltype(work);
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<transfer>,
                                execution::completion_signatures<execution::set_value_t(int),
                                                                 execution::set_error_t(int)>>);
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<transfer, alternate_env>,
                                execution::completion_signatures<execution::set_value_t(int, int),
                                                                 execution::set_error_t(long)>>);
    STATIC_REQUIRE_FALSE(execution::sender_in<transfer, unavailable_env>);
    // Scheduling accepts this environment, but copying the referenced payload
    // into storage may throw.
    using storage_transfer =
        decltype(execution::continues_on(environment_storage_source{}, environment_scheduler{}));
    STATIC_REQUIRE_FALSE(execution::sender_in<storage_transfer, unstorable_env>);
    int completion = 0;
    auto operation = execution::connect(work, pair_receiver{.result = &completion});
    execution::start(operation);
    REQUIRE(completion == -73);
}
