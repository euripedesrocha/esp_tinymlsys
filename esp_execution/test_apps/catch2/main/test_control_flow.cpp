// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <concepts>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution.hpp"

namespace {

namespace execution = esp::execution;
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

struct throwing_error_handler {
    void operator()(int) const;
};

struct throwing_stopped_handler {
    void operator()() const;
};

struct multi_error_sender {
    using sender_concept = execution::sender_tag;

    template <class Env>
    using completion_signatures =
        execution::completion_signatures<execution::set_value_t(), execution::set_error_t(int),
                                         execution::set_error_t(long), execution::set_stopped_t()>;
};

struct multi_error_handler {
    void operator()(int) const noexcept {}
    void operator()(long) const noexcept {}
};

struct stopped_handler {
    void operator()() const noexcept {}
};
template <class Sender, class Callable>
concept can_upon_error = requires(Sender&& sender, Callable&& callable) {
    execution::upon_error(std::forward<Sender>(sender), std::forward<Callable>(callable));
};

template <class Sender, class Callable>
concept can_upon_stopped = requires(Sender&& sender, Callable&& callable) {
    execution::upon_stopped(std::forward<Sender>(sender), std::forward<Callable>(callable));
};

struct continuation_test_env {};
struct unavailable_env {};

template <class Tag> struct environment_dependent_sender {
    using sender_concept = execution::sender_tag;

    template <class Env>
        requires(!std::same_as<Env, unavailable_env>)
    using completion_signatures = std::conditional_t<
        std::same_as<Tag, execution::set_stopped_t>,
        std::conditional_t<std::same_as<Env, continuation_test_env>,
                           execution::completion_signatures<execution::set_stopped_t()>,
                           execution::completion_signatures<execution::set_value_t(int)>>,
        execution::completion_signatures<Tag(
            std::conditional_t<std::same_as<Env, continuation_test_env>, long, int>)>>;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        Receiver receiver;

        void start() & noexcept {
            if constexpr (std::same_as<execution::env_of_t<Receiver>, continuation_test_env>) {
                if constexpr (std::same_as<Tag, execution::set_stopped_t>) {
                    Tag{}(std::move(receiver));
                } else {
                    Tag{}(std::move(receiver), 1L);
                }
            } else if constexpr (std::same_as<Tag, execution::set_stopped_t>) {
                execution::set_value(std::move(receiver), 1);
            } else {
                Tag{}(std::move(receiver), 1);
            }
        }
    };

    auto connect(execution::receiver auto receiver) const {
        return operation<decltype(receiver)>{std::move(receiver)};
    }
};

template <class Env> struct environment_receiver : result_receiver {
    auto get_env() const noexcept -> Env { return {}; }
};

struct environment_handler {
    int operator()(int) const noexcept { return 1; }
    int operator()(long) const noexcept { return 42; }
    int operator()() const noexcept { return 42; }
};

struct default_environment_handler {
    int operator()(int) const noexcept { return 1; }
    int operator()(long) const = delete;
    int operator()() const = delete;
};

struct throwing_environment_handler {
    int operator()(int) const noexcept { return 1; }
    int operator()(long) const noexcept(false) { return 42; }
    int operator()() const noexcept(false) { return 42; }
};

template <class Sender, class Receiver>
concept can_connect = requires(Sender&& sender, Receiver&& receiver) {
    execution::connect(std::forward<Sender>(sender), std::forward<Receiver>(receiver));
};

template <class Tag> void check_receiver_environment(auto adaptor) {
    using source = environment_dependent_sender<Tag>;
    using valid = decltype(adaptor(source{}, environment_handler{}));
    using incompatible = decltype(adaptor(source{}, default_environment_handler{}));
    using throwing = decltype(adaptor(source{}, throwing_environment_handler{}));
    using receiver = environment_receiver<continuation_test_env>;

    STATIC_REQUIRE(execution::sender_in<valid, continuation_test_env>);
    STATIC_REQUIRE(execution::sender_in<incompatible>);
    STATIC_REQUIRE(execution::sender_in<throwing>);
    STATIC_REQUIRE_FALSE(execution::sender_in<incompatible, continuation_test_env>);
    STATIC_REQUIRE_FALSE(execution::sender_in<throwing, continuation_test_env>);
    STATIC_REQUIRE_FALSE(execution::sender_in<valid, unavailable_env>);
    STATIC_REQUIRE_FALSE(can_connect<valid, environment_receiver<unavailable_env>>);
    STATIC_REQUIRE_FALSE(can_connect<const valid&, environment_receiver<unavailable_env>>);
    STATIC_REQUIRE_FALSE(can_connect<incompatible, receiver>);
    STATIC_REQUIRE_FALSE(can_connect<const incompatible&, receiver>);
    STATIC_REQUIRE_FALSE(can_connect<throwing, receiver>);
    STATIC_REQUIRE_FALSE(can_connect<const throwing&, receiver>);

    result completion;
    auto work = adaptor(source{}, environment_handler{});
    SECTION("rvalue connection") {
        auto operation = execution::connect(std::move(work), receiver{{.state = &completion}});
        execution::start(operation);
    }
    SECTION("const lvalue connection") {
        auto operation = execution::connect(std::as_const(work), receiver{{.state = &completion}});
        execution::start(operation);
    }
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

} // namespace
TEST_CASE("upon_error handles errors as value completions", "[upon_error]") {
    result completion;
    int handled = 0;
    auto work = execution::just_error(17) | execution::upon_error([&handled](int error) noexcept {
                    handled = error;
                    return 42;
                });
    auto operation = execution::connect(std::move(work), result_receiver{&completion});

    execution::start(operation);

    REQUIRE(handled == 17);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
    REQUIRE(completion.errors == 0);
    STATIC_REQUIRE_FALSE(
        can_upon_error<decltype(execution::just_error(1)), throwing_error_handler>);
}

TEST_CASE("upon_error deduplicates transformed signatures", "[upon_error][signatures]") {
    using transformed_sender =
        decltype(execution::upon_error(multi_error_sender{}, multi_error_handler{}));

    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<transformed_sender>,
                                execution::completion_signatures<execution::set_value_t(),
                                                                 execution::set_stopped_t()>>);
}

TEST_CASE("upon_error leaves values and stopped completions unchanged", "[upon_error]") {
    result value_completion;
    result stopped_completion;
    auto handler = [](int) noexcept { return 0; };
    auto value_work = execution::just(42) | execution::upon_error(handler);
    auto stopped_work = execution::just_stopped() | execution::upon_error(handler);
    auto value_operation =
        execution::connect(std::move(value_work), result_receiver{&value_completion});
    auto stopped_operation =
        execution::connect(std::move(stopped_work), result_receiver{&stopped_completion});

    execution::start(value_operation);
    execution::start(stopped_operation);

    REQUIRE(value_completion.values == 1);
    REQUIRE(value_completion.value == 42);
    REQUIRE(stopped_completion.stopped == 1);
}

TEST_CASE("upon_stopped handles stopped as a value completion", "[upon_stopped]") {
    result completion;
    int handled = 0;
    auto work = execution::just_stopped() | execution::upon_stopped([&handled]() noexcept {
                    handled += 1;
                    return 42;
                });
    auto operation = execution::connect(std::move(work), result_receiver{&completion});

    execution::start(operation);

    REQUIRE(handled == 1);
    REQUIRE(completion.values == 1);
    REQUIRE(completion.value == 42);
    REQUIRE(completion.stopped == 0);
    STATIC_REQUIRE_FALSE(
        can_upon_stopped<decltype(execution::just_stopped()), throwing_stopped_handler>);
}

TEST_CASE("upon_stopped deduplicates transformed signatures", "[upon_stopped][signatures]") {
    using transformed_sender =
        decltype(execution::upon_stopped(multi_error_sender{}, stopped_handler{}));

    STATIC_REQUIRE(
        std::same_as<
            execution::completion_signatures_of_t<transformed_sender>,
            execution::completion_signatures<execution::set_value_t(), execution::set_error_t(int),
                                             execution::set_error_t(long)>>);
}

TEST_CASE("upon_stopped leaves values and errors unchanged", "[upon_stopped]") {
    result value_completion;
    result error_completion;
    int handled = 0;
    auto handler = [&handled]() noexcept {
        handled += 1;
        return 0;
    };
    auto value_work = execution::just(42) | execution::upon_stopped(handler);
    auto error_work = execution::just_error(17) | execution::upon_stopped(handler);
    auto value_operation =
        execution::connect(std::move(value_work), result_receiver{&value_completion});
    auto error_operation =
        execution::connect(std::move(error_work), result_receiver{&error_completion});

    execution::start(value_operation);
    execution::start(error_operation);

    REQUIRE(handled == 0);
    REQUIRE(value_completion.values == 1);
    REQUIRE(value_completion.value == 42);
    REQUIRE(error_completion.errors == 1);
    REQUIRE(error_completion.error == 17);
}

TEST_CASE("then validates its callable against the receiver environment", "[then][environment]") {
    check_receiver_environment<execution::set_value_t>(execution::then);
}

TEST_CASE("upon_error validates its callable against the receiver environment",
          "[upon_error][environment]") {
    check_receiver_environment<execution::set_error_t>(execution::upon_error);
}

TEST_CASE("upon_stopped validates its callable against the receiver environment",
          "[upon_stopped][environment]") {
    check_receiver_environment<execution::set_stopped_t>(execution::upon_stopped);
}
