// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/then.hpp"

#include "deferred_sender_test_support.hpp"
#include "repeat_test_support.hpp"

#include <catch2/generators/catch_generators.hpp>

namespace {

using namespace esp_execution_test;

template <class Source, class Callable>
concept can_let_value = requires(Source&& source, Callable&& callable) {
    execution::let_value(std::forward<Source>(source), std::forward<Callable>(callable));
};

template <class Source, class Callable>
concept can_form_sender =
    requires { typename execution::detail::let_value::sender<Source, Callable>; };

template <class Sender, class Receiver>
concept can_connect = requires(Sender&& source, Receiver&& receiver) {
    execution::connect(std::forward<Sender>(source), std::forward<Receiver>(receiver));
};

template <class... Signatures> struct signature_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = execution::completion_signatures<Signatures...>;
};

struct noninvocable_callable {};

struct copy_only_callable {
    copy_only_callable() = default;
    copy_only_callable(const copy_only_callable&) = default;
    copy_only_callable(copy_only_callable&&) = delete;
    auto operator()(int&) const noexcept { return execution::just(); }
};

struct throwing_copy {
    throwing_copy(const throwing_copy&) noexcept(false);
    throwing_copy(throwing_copy&&) noexcept = default;
};

struct error_only_receiver {
    using receiver_concept = execution::receiver_tag;
    void set_error(int) && noexcept {}
};

struct value_shape_callable {
    int* calls;
    auto operator()(int& value) const noexcept {
        ++*calls;
        return execution::just(value + 41);
    }
    auto operator()(int& first, int& second) const noexcept {
        ++*calls;
        return execution::just_error(first + second + 39);
    }
};

struct alternate_env {};
struct unsupported_source_env {};
struct unsupported_child_env {};

struct environment_child {
    using sender_concept = execution::sender_tag;
    template <class Env>
        requires(!std::same_as<Env, unsupported_child_env>)
    using completion_signatures =
        std::conditional_t<std::same_as<Env, alternate_env>,
                           execution::completion_signatures<execution::set_value_t(int, int)>,
                           execution::completion_signatures<execution::set_value_t(int)>>;

    int value;

    auto connect(execution::receiver auto receiver) const {
        if constexpr (std::same_as<execution::env_of_t<decltype(receiver)>, alternate_env>) {
            return execution::connect(execution::just(value - 1, 1), std::move(receiver));
        } else {
            return execution::connect(execution::just(value), std::move(receiver));
        }
    }
};

struct environment_source {
    using sender_concept = execution::sender_tag;
    alternate_env* environment;

    const alternate_env& get_env() const noexcept { return *environment; }

    template <class Env>
    using completion_signatures = std::conditional_t<
        std::same_as<Env, alternate_env>,
        execution::completion_signatures<execution::set_value_t(int, int)>,
        std::conditional_t<std::same_as<Env, unsupported_source_env>,
                           execution::completion_signatures<execution::set_value_t(const char*)>,
                           execution::completion_signatures<execution::set_value_t(int)>>>;

    auto connect(execution::receiver auto receiver) const {
        using env = execution::env_of_t<decltype(receiver)>;
        if constexpr (std::same_as<env, alternate_env>) {
            return execution::connect(execution::just(20, 22), std::move(receiver));
        } else if constexpr (std::same_as<env, unsupported_source_env>) {
            const char* value = "unsupported";
            return execution::connect(execution::just(value), std::move(receiver));
        } else {
            return execution::connect(execution::just(42), std::move(receiver));
        }
    }
};

struct environment_callable {
    auto operator()(int& value) const noexcept { return environment_child{value}; }
    auto operator()(int& first, int& second) const noexcept {
        return environment_child{first + second};
    }
};

template <class Env> struct environment_receiver {
    using receiver_concept = execution::receiver_tag;
    int* value;
    Env get_env() const noexcept { return {}; }
    void set_value(int first, int second) && noexcept { *value = first + second; }
};

enum class input_kind { value, const_reference };

struct same_decay_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(int),
                                         execution::set_value_t(const int&)>;

    const int* input;
    input_kind kind;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        const int* input;
        input_kind kind;
        Receiver receiver;

        void start() & noexcept {
            if (kind == input_kind::value) {
                execution::set_value(std::move(receiver), int{*input});
            } else {
                execution::set_value(std::move(receiver), *input);
            }
        }
    };

    auto connect(execution::receiver auto receiver) const {
        return operation<decltype(receiver)>{
            .input = input, .kind = kind, .receiver = std::move(receiver)};
    }
};

struct tracked_value {
    int value;
    int* destructions;
    ~tracked_value() { ++*destructions; }
};

// Record observations during child destruction, without assertions in a destructor.
template <class Observer> struct destruction_observed_sender {
    using sender_concept = execution::sender_tag;
    template <class Env> using completion_signatures = deferred_sender::completion_signatures<Env>;

    deferred_completion* pending;
    Observer observe_destruction;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        Observer observe_destruction;
        decltype(execution::connect(std::declval<deferred_sender>(),
                                    std::declval<Receiver>())) child;

        ~operation() { observe_destruction(); }
        void start() & noexcept { execution::start(child); }
    };

    auto connect(execution::receiver auto receiver) const {
        return operation<decltype(receiver)>{
            .observe_destruction = observe_destruction,
            .child = execution::connect(deferred_sender{.pending = pending}, std::move(receiver))};
    }
};

struct rvalue_child_sender {
    using sender_concept = execution::sender_tag;
    template <class Env>
    using completion_signatures = execution::completion_signatures<execution::set_value_t(int)>;

    std::unique_ptr<int> value;

    auto connect(execution::receiver auto receiver) && {
        return execution::connect(execution::just(*value), std::move(receiver));
    }
};

struct rvalue_callable {
    std::unique_ptr<int> offset;

    auto operator()(int& value) && noexcept {
        *offset += value;
        return rvalue_child_sender{.value = std::move(offset)};
    }
};

} // namespace

TEST_CASE("let_value retains values and starts the returned sender", "[let_value]") {
    result completion;
    int observed = 0;
    auto work = execution::just(40) | execution::let_value([&observed](int& value) noexcept {
                    observed = value;
                    return execution::just(value + 2);
                });
    auto operation = execution::connect(std::move(work), result_receiver{&completion});

    execution::start(operation);

    REQUIRE(observed == 40);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
}

TEST_CASE("let_value forwards predecessor and child terminal completions", "[let_value]") {
    int starts = 0;
    int callable_calls = 0;
    result predecessor_error;
    result child_error;
    result child_stopped;

    auto predecessor = iteration_sender{&starts, 0, terminate_as::error} |
                       execution::let_value([&callable_calls]() noexcept {
                           callable_calls += 1;
                           return execution::just();
                       });
    auto error_child = execution::just() |
                       execution::let_value([]() noexcept { return execution::just_error(23); });
    auto stopped_child = execution::just() |
                         execution::let_value([]() noexcept { return execution::just_stopped(); });
    auto predecessor_operation =
        execution::connect(std::move(predecessor), result_receiver{&predecessor_error});
    auto error_operation =
        execution::connect(std::move(error_child), result_receiver{&child_error});
    auto stopped_operation =
        execution::connect(std::move(stopped_child), result_receiver{&child_stopped});

    execution::start(predecessor_operation);
    execution::start(error_operation);
    execution::start(stopped_operation);

    REQUIRE(callable_calls == 0);
    REQUIRE(predecessor_error.errors == 1);
    REQUIRE(child_error.errors == 1);
    REQUIRE(child_error.error == 23);
    REQUIRE(child_stopped.stopped == 1);
}

TEST_CASE("let_value selects different child senders for every value shape",
          "[let_value][signatures]") {
    const auto iteration = GENERATE(1, 2);
    int starts = iteration - 1;
    int calls = 0;
    result completion;
    auto work = multi_value_sender{.starts = &starts} |
                execution::let_value(value_shape_callable{.calls = &calls});
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t(int),
                                                                 execution::set_error_t(int),
                                                                 execution::set_stopped_t()>>);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    execution::start(operation);
    REQUIRE(calls == 1);
    REQUIRE(completion.values == (iteration == 1 ? 1 : 0));
    REQUIRE(completion.errors == (iteration == 2 ? 1 : 0));
    REQUIRE(completion.stopped == 0);
    REQUIRE((iteration == 1 ? completion.value : completion.error) == 42);
}

TEST_CASE("let_value bypasses its callable when no value signatures exist", "[let_value]") {
    result error;
    result stopped;
    auto error_work = execution::just_error(17) | execution::let_value(noninvocable_callable{});
    auto stopped_work = execution::just_stopped() | execution::let_value(noninvocable_callable{});
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(error_work)>,
                                execution::completion_signatures<execution::set_error_t(int)>>);
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(stopped_work)>,
                                execution::completion_signatures<execution::set_stopped_t()>>);
    auto error_operation =
        execution::connect(std::move(error_work), result_receiver{.state = &error});
    auto stopped_operation =
        execution::connect(std::move(stopped_work), result_receiver{.state = &stopped});
    execution::start(error_operation);
    execution::start(stopped_operation);
    REQUIRE(error.errors == 1);
    REQUIRE(error.error == 17);
    REQUIRE(error.values == 0);
    REQUIRE(stopped.stopped == 1);
    REQUIRE(stopped.values == 0);
}

TEST_CASE("let_value shares same-decay branches and retains independent reference input",
          "[let_value][signatures][ownership]") {
    const auto kind = GENERATE(input_kind::value, input_kind::const_reference);
    int original = 40;
    int* retained_address = nullptr;
    deferred_completion pending;
    result completion;
    auto work = same_decay_sender{.input = &original, .kind = kind} |
                execution::let_value([&](int& retained) noexcept {
                    retained_address = &retained;
                    retained += 2;
                    return deferred_sender{.pending = &pending} |
                           execution::then([&retained]() noexcept { return retained; });
                });
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t(int),
                                                                 execution::set_error_t(int),
                                                                 execution::set_stopped_t()>>);
    auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
    execution::start(operation);
    REQUIRE(pending.starts == 1);
    REQUIRE(retained_address != &original);
    REQUIRE(original == 40);
    REQUIRE(completion.values == 0);
    original = 99;
    pending.deliver(completion_kind::value);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
    REQUIRE(original == 99);
}

TEST_CASE("let_value rejects invalid callables without hard substitution errors",
          "[let_value][constraints]") {
    using source = decltype(execution::just(42));
    auto valid = [](int&) noexcept { return execution::just(); };
    auto throwing = [](int&) { return execution::just(); };
    auto not_a_sender = [](int&) noexcept { return 42; };
    auto rvalue_argument = [](int&&) noexcept { return execution::just(); };
    auto invalid_signatures = [](int&) noexcept { return signature_sender<int>{}; };
    auto accepts_any = [](auto&...) noexcept { return execution::just(); };
    STATIC_REQUIRE(can_let_value<source, decltype(valid)>);
    STATIC_REQUIRE_FALSE(can_let_value<int, decltype(valid)>);
    STATIC_REQUIRE_FALSE(can_let_value<source, noninvocable_callable>);
    STATIC_REQUIRE_FALSE(can_let_value<source, decltype(throwing)>);
    STATIC_REQUIRE_FALSE(can_let_value<source, decltype(not_a_sender)>);
    STATIC_REQUIRE_FALSE(can_let_value<source, decltype(rvalue_argument)>);
    STATIC_REQUIRE_FALSE(can_let_value<source, decltype(invalid_signatures)>);
    STATIC_REQUIRE_FALSE(can_let_value<source, copy_only_callable>);
    STATIC_REQUIRE_FALSE(can_let_value<multi_value_sender, decltype(valid)>);
    STATIC_REQUIRE_FALSE(can_let_value<signature_sender<execution::set_value_t(throwing_copy&)>,
                                       decltype(accepts_any)>);
}

TEST_CASE("let_value establishes compatibility as a sender class invariant",
          "[let_value][constraints]") {
    using source = decltype(execution::just(42));
    auto valid = [](int& value) noexcept { return execution::just(value); };
    auto throwing = [](int&) { return execution::just(); };
    auto not_a_sender = [](int&) noexcept { return 42; };
    STATIC_REQUIRE(can_form_sender<source, decltype(valid)>);
    STATIC_REQUIRE_FALSE(can_form_sender<int, decltype(valid)>);
    STATIC_REQUIRE_FALSE(can_form_sender<source, noninvocable_callable>);
    STATIC_REQUIRE_FALSE(can_form_sender<source, decltype(throwing)>);
    STATIC_REQUIRE_FALSE(can_form_sender<source, decltype(not_a_sender)>);
    STATIC_REQUIRE_FALSE(can_form_sender<source, copy_only_callable>);
    STATIC_REQUIRE_FALSE(can_form_sender<multi_value_sender, decltype(valid)>);
    STATIC_REQUIRE(can_form_sender<decltype(execution::just_error(1)), noninvocable_callable>);
    using work = decltype(execution::let_value(std::declval<source>(), valid));
    STATIC_REQUIRE(can_connect<work, result_receiver>);
    STATIC_REQUIRE(can_connect<const work&, result_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<work, error_only_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<const work&, error_only_receiver>);
}

TEST_CASE("let_value validates every branch against the receiver environment",
          "[let_value][environment]") {
    alternate_env environment;
    auto work = execution::let_value(environment_source{.environment = &environment},
                                     environment_callable{});
    using sender = decltype(work);
    STATIC_REQUIRE(std::same_as<decltype(execution::get_env(work)), const alternate_env&>);
    REQUIRE(std::addressof(execution::get_env(work)) == std::addressof(environment));
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<sender>,
                                execution::completion_signatures<execution::set_value_t(int)>>);
    STATIC_REQUIRE(
        std::same_as<execution::completion_signatures_of_t<sender, alternate_env>,
                     execution::completion_signatures<execution::set_value_t(int, int)>>);
    STATIC_REQUIRE_FALSE(execution::sender_in<sender, unsupported_source_env>);
    STATIC_REQUIRE_FALSE(execution::sender_in<sender, unsupported_child_env>);
    STATIC_REQUIRE_FALSE(can_connect<sender, environment_receiver<unsupported_source_env>>);
    STATIC_REQUIRE_FALSE(can_connect<sender, environment_receiver<unsupported_child_env>>);
    STATIC_REQUIRE_FALSE(can_connect<const sender&, environment_receiver<unsupported_source_env>>);
    STATIC_REQUIRE_FALSE(can_connect<const sender&, environment_receiver<unsupported_child_env>>);

    int value = 0;
    auto operation = execution::connect(work, environment_receiver<alternate_env>{.value = &value});
    execution::start(operation);
    REQUIRE(value == 42);
}

TEST_CASE("let_value retains move-only input until its deferred child is destroyed",
          "[let_value][ownership][lifecycle]") {
    int destructions = 0;
    int value_at_destruction = 0;
    deferred_completion pending;
    result completion;
    {
        auto input = std::unique_ptr<tracked_value>{
            new tracked_value{.value = 42, .destructions = &destructions}};
        auto work = execution::just(std::move(input)) |
                    execution::let_value([&](std::unique_ptr<tracked_value>& retained) noexcept {
                        return destruction_observed_sender{
                            .pending = &pending,
                            .observe_destruction = [&retained, &value_at_destruction]() noexcept {
                                value_at_destruction = retained->value;
                            }};
                    });
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        execution::start(operation);
        REQUIRE(pending.starts == 1);
        REQUIRE(destructions == 0);
        REQUIRE(completion.values == 0);

        SECTION("completed child retains its input until operation destruction") {
            pending.deliver(completion_kind::value);
            REQUIRE(completion.values == 1);
        }
        SECTION("destroying an incomplete child preserves its borrowed input") {
            REQUIRE(pending.operation != nullptr);
        }
        REQUIRE(destructions == 0);
        REQUIRE(value_at_destruction == 0);
    }
    REQUIRE(value_at_destruction == 42);
    REQUIRE(destructions == 1);
}

TEST_CASE("let_value destroys its child before the upstream operation", "[let_value][lifecycle]") {
    int starts = 0;
    child_lifetime upstream_lifetime;
    int upstream_live_at_child_destruction = -1;
    deferred_completion pending;
    result completion;
    auto work = iteration_sender{.iterations_started = &starts,
                                 .iterations_before_terminal = 1,
                                 .terminal = terminate_as::error,
                                 .lifetime = &upstream_lifetime} |
                execution::let_value([&]() noexcept {
                    return destruction_observed_sender{
                        .pending = &pending, .observe_destruction = [&]() noexcept {
                            upstream_live_at_child_destruction = upstream_lifetime.live;
                        }};
                });

    SECTION("scoped operation") {
        {
            auto operation = execution::connect(work, result_receiver{.state = &completion});
            execution::start(operation);
            pending.deliver(completion_kind::value);
            REQUIRE(upstream_lifetime.live == 1);
            REQUIRE(upstream_live_at_child_destruction == -1);
        }
    }
    SECTION("receiver destroys the operation") {
        completion = run_and_destroy(std::move(work), [&](auto& operation) {
            execution::start(operation);
            REQUIRE(upstream_lifetime.live == 1);
            pending.deliver(completion_kind::value);
        });
    }
    REQUIRE(completion.values == 1);
    REQUIRE(upstream_live_at_child_destruction == 1);
    REQUIRE(upstream_lifetime.live == 0);
    REQUIRE(upstream_lifetime.constructed == 1);
    REQUIRE(upstream_lifetime.destroyed == 1);
}

TEST_CASE("let_value permits its receiver to destroy the operation during completion",
          "[let_value][lifecycle]") {
    SECTION("synchronous child") {
        const auto completion = complete_and_destroy<value_destroying_receiver>(
            execution::just(40) |
            execution::let_value([](int& value) noexcept { return execution::just(value + 2); }));
        REQUIRE(completion.values == 1);
        REQUIRE(completion.value == 42);
    }
    SECTION("deferred child") {
        const auto kind =
            GENERATE(completion_kind::value, completion_kind::error, completion_kind::stopped);
        deferred_completion input;
        deferred_completion child;
        auto work = deferred_sender{.pending = &input} | execution::let_value([&child]() noexcept {
                        return deferred_sender{.pending = &child};
                    });
        const auto completion = run_and_destroy(std::move(work), [&](auto& operation) {
            execution::start(operation);
            input.deliver(completion_kind::value);
            REQUIRE(child.starts == 1);
            child.deliver(kind);
        });
        REQUIRE(completion.values == (kind == completion_kind::value ? 1 : 0));
        REQUIRE(completion.errors == (kind == completion_kind::error ? 1 : 0));
        REQUIRE(completion.stopped == (kind == completion_kind::stopped ? 1 : 0));
    }
    SECTION("upstream terminal completion") {
        const auto kind = GENERATE(completion_kind::error, completion_kind::stopped);
        deferred_completion input;
        deferred_completion child;
        auto work = deferred_sender{.pending = &input} | execution::let_value([&child]() noexcept {
                        return deferred_sender{.pending = &child};
                    });
        const auto completion = run_and_destroy(std::move(work), [&](auto& operation) {
            execution::start(operation);
            input.deliver(kind);
        });
        REQUIRE(child.starts == 0);
        REQUIRE(completion.values == 0);
        REQUIRE(completion.errors == (kind == completion_kind::error ? 1 : 0));
        REQUIRE(completion.stopped == (kind == completion_kind::stopped ? 1 : 0));
    }
}

TEST_CASE("let_value owns move-only callables and repeats reusable pipelines",
          "[let_value][ownership][repeat]") {
    SECTION("move-only callable") {
        result completion;
        auto callable = rvalue_callable{.offset = std::make_unique<int>(2)};
        auto work = execution::just(40) | execution::let_value(std::move(callable));
        STATIC_REQUIRE_FALSE(std::copy_constructible<decltype(work)>);
        STATIC_REQUIRE(can_connect<decltype(work), result_receiver>);
        STATIC_REQUIRE_FALSE(can_connect<const decltype(work)&, result_receiver>);
        auto operation = execution::connect(std::move(work), result_receiver{.state = &completion});
        execution::start(operation);
        REQUIRE(completion.values == 1);
        REQUIRE(completion.value == 42);
    }
    SECTION("reusable predecessor and callable") {
        int starts = 0;
        int calls = 0;
        result completion;
        auto work = numbered_iteration_sender{.iterations_started = &starts,
                                              .iterations_before_terminal = 3,
                                              .terminal = terminate_as::error} |
                    execution::let_value([&calls](int& value) noexcept {
                        ++calls;
                        return execution::just(value);
                    }) |
                    execution::repeat();
        auto operation = execution::connect(work, result_receiver{.state = &completion});
        execution::start(operation);
        REQUIRE(starts == 4);
        REQUIRE(calls == 3);
        REQUIRE(completion.values == 0);
        REQUIRE(completion.errors == 1);
        REQUIRE(completion.error == 4);
    }
}
