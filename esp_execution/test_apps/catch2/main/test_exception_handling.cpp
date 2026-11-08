// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/then.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"
#include "esp_execution_test/execution.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <exception>
#include <functional>

namespace {
using namespace esp_execution_test;
struct failure {
    int stage;
};

result run(execution::sender auto source) {
    result completion;
    auto operation = execution::connect(std::move(source), result_receiver{&completion});
    execution::start(operation);
    return completion;
}

void check_exception(const result& completion, int stage) {
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 1);
    REQUIRE(completion.stopped == 0);
    REQUIRE(completion.exception);
    try {
        std::rethrow_exception(completion.exception);
    } catch (const failure& error) {
        REQUIRE(error.stage == stage);
    }
}

struct retained_input {
    int* live;
    bool fail_copy;
    retained_input(int& count, bool fail) : live(&count), fail_copy(fail) { ++*live; }
    retained_input(const retained_input& other) : live(other.live), fail_copy(other.fail_copy) {
        if (fail_copy)
            throw failure{1};
        ++*live;
    }
    ~retained_input() { --*live; }
};

struct connection_sender {
    using sender_concept = execution::sender_tag;
    int* connections;
    int* starts;
    bool fail;
    int fail_at = 1;
    template <class Env>
    using completion_signatures = execution::completion_signatures<execution::set_value_t(int)>;
    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        Receiver receiver;
        int* starts;
        void start() & noexcept {
            ++*starts;
            execution::set_value(std::move(receiver), 42);
        }
    };
    auto connect(execution::receiver auto receiver) const {
        ++*connections;
        if (fail && *connections == fail_at)
            throw failure{3};
        return operation<decltype(receiver)>{std::move(receiver), starts};
    }
};

struct throwing_storage_callable {
    bool* fail_copy;
    bool* fail_move;

    throwing_storage_callable(bool& copy, bool& move) noexcept
        : fail_copy(&copy), fail_move(&move) {}
    throwing_storage_callable(const throwing_storage_callable& other)
        : fail_copy(other.fail_copy), fail_move(other.fail_move) {
        if (*fail_copy)
            throw failure{4};
    }
    throwing_storage_callable(throwing_storage_callable&& other)
        : fail_copy(other.fail_copy), fail_move(other.fail_move) {
        if (*fail_move)
            throw failure{5};
    }
    int operator()(auto&&...) && noexcept { return 42; }
};

void check_callable_storage(auto adaptor, execution::sender auto source) {
    bool fail_copy = false, fail_move = false;
    auto work = adaptor(std::move(source), throwing_storage_callable{fail_copy, fail_move});
    STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                execution::completion_signatures<execution::set_value_t(int)>>);
    const bool copy = GENERATE(false, true);
    const bool fail = GENERATE(false, true);
    fail_copy = copy && fail;
    fail_move = !copy && fail;
    result completion;
    auto check_connection = [&](auto&& sender) {
        if (fail) {
            REQUIRE_THROWS_AS(execution::connect(std::forward<decltype(sender)>(sender),
                                                 result_receiver{&completion}),
                              failure);
            REQUIRE(completion.values == 0);
        } else {
            auto operation = execution::connect(std::forward<decltype(sender)>(sender),
                                                result_receiver{&completion});
            REQUIRE(completion.values == 0);
            execution::start(operation);
            REQUIRE(completion.values == 1);
            REQUIRE(completion.value == 42);
        }
    };
    if (copy)
        check_connection(std::as_const(work));
    else
        check_connection(std::move(work));
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

struct throwing_copy_receiver : result_receiver {
    bool* fail_copy;
    throwing_copy_receiver(result& completion, bool& fail) noexcept
        : result_receiver{&completion}, fail_copy(&fail) {}
    throwing_copy_receiver(const throwing_copy_receiver& other)
        : result_receiver(other), fail_copy(other.fail_copy) {
        if (*fail_copy)
            throw failure{6};
    }
    throwing_copy_receiver(throwing_copy_receiver&&) noexcept = default;
};

void check_receiver_copy(execution::sender auto work) {
    result completion;
    bool fail = GENERATE(false, true);
    throwing_copy_receiver receiver{completion, fail};
    STATIC_REQUIRE(execution::receiver<throwing_copy_receiver&>);
    STATIC_REQUIRE_FALSE(noexcept(execution::connect(std::move(work), receiver)));
    STATIC_REQUIRE(noexcept(execution::connect(std::move(work), std::move(receiver))));
    if (fail) {
        REQUIRE_THROWS_AS(execution::connect(std::move(work), receiver), failure);
        REQUIRE(completion.values == 0);
    } else {
        auto operation = execution::connect(std::move(work), receiver);
        REQUIRE(completion.values == 0);
        execution::start(operation);
        REQUIRE(completion.values == 1);
    }
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.stopped == 0);
}

struct connection_env {};
struct noncopyable_env {
    noncopyable_env() = default;
    noncopyable_env(const noncopyable_env&) = delete;
};

template <class Env, bool Alternate = false> struct connection_receiver : result_receiver {
    std::remove_cvref_t<Env>* environment;
    Env get_env() const noexcept { return *environment; }
};

struct environment_connection_sender {
    using sender_concept = execution::sender_tag;
    bool fail;
    template <class Env>
    using completion_signatures = execution::completion_signatures<execution::set_value_t(int)>;
    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        Receiver receiver;
        void start() & noexcept { execution::set_value(std::move(receiver), 42); }
    };
    auto connect(execution::receiver auto receiver) const
        noexcept(std::is_reference_v<execution::env_of_t<decltype(receiver)>>) {
        if constexpr (!std::is_reference_v<execution::env_of_t<decltype(receiver)>>) {
            if (fail)
                throw failure{7};
        }
        return operation<decltype(receiver)>{std::move(receiver)};
    }
};

template <class Env> void check_connection_environment() {
    using first_receiver = connection_receiver<Env>;
    using second_receiver = connection_receiver<Env, true>;
    std::remove_cvref_t<Env> environment;
    result completion;
    const bool fail = GENERATE(false, true);
    environment_connection_sender source{fail};
    STATIC_REQUIRE(noexcept(execution::connect(source, std::declval<first_receiver>())) ==
                   noexcept(execution::connect(source, std::declval<second_receiver>())));
    auto work = execution::just() | execution::let_value([source]() noexcept { return source; });
    using signatures = execution::completion_signatures_of_t<decltype(work), Env>;
    if constexpr (std::is_reference_v<Env>) {
        STATIC_REQUIRE(std::same_as<signatures,
                                    execution::completion_signatures<execution::set_value_t(int)>>);
    } else {
        STATIC_REQUIRE(std::same_as<signatures, execution::completion_signatures<
                                                    execution::set_value_t(int),
                                                    execution::set_error_t(std::exception_ptr)>>);
    }
    auto operation =
        execution::connect(std::move(work), first_receiver{{&completion}, &environment});
    REQUIRE(completion.values == 0);
    REQUIRE(completion.errors == 0);
    execution::start(operation);
    if (!std::is_reference_v<Env> && fail)
        check_exception(completion, 7);
    else {
        REQUIRE(completion.values == 1);
        REQUIRE(completion.value == 42);
        REQUIRE(completion.errors == 0);
    }
}
} // namespace

TEST_CASE("continuations own potentially throwing callables outside their internal receivers",
          "[then][upon_error][upon_stopped][exceptions]") {
    SECTION("then") { check_callable_storage(execution::then, execution::just(1)); }
    SECTION("upon_error") {
        check_callable_storage(execution::upon_error, execution::just_error(1));
    }
    SECTION("upon_stopped") {
        check_callable_storage(execution::upon_stopped, execution::just_stopped());
    }
}

TEST_CASE("initial connection may copy a nonthrowing movable receiver", "[connect][exceptions]") {
    SECTION("just") { check_receiver_copy(execution::just(42)); }
    SECTION("then") {
        check_receiver_copy(execution::just(1) | execution::then([](int) noexcept { return 42; }));
    }
    SECTION("let_value") {
        check_receiver_copy(execution::just() |
                            execution::let_value([]() noexcept { return execution::just(42); }));
    }
    SECTION("repeat_n") { check_receiver_copy(execution::repeat_n(execution::just(), 1)); }
}

TEST_CASE("dependent connection guarantees use the exact receiver environment",
          "[let_value][connect][environment][exceptions]") {
    SECTION("value") { check_connection_environment<connection_env>(); }
    SECTION("reference") { check_connection_environment<connection_env&>(); }
    SECTION("const reference") { check_connection_environment<const connection_env&>(); }
    SECTION("noncopyable reference") { check_connection_environment<noncopyable_env&>(); }
}

TEST_CASE("continuations report callable exceptions through set_error",
          "[then][upon_error][upon_stopped][exceptions]") {
    SECTION("then with a value result") {
        auto work = execution::just(1) | execution::then([](int) -> int { throw failure{1}; });
        STATIC_REQUIRE(
            std::same_as<
                execution::completion_signatures_of_t<decltype(work)>,
                execution::completion_signatures<execution::set_value_t(int),
                                                 execution::set_error_t(std::exception_ptr)>>);
        check_exception(run(std::move(work)), 1);
    }
    SECTION("then with a void result") {
        check_exception(run(execution::just() | execution::then([] { throw failure{2}; })), 2);
    }
    SECTION("upon_error") {
        check_exception(run(execution::just_error(1) |
                            execution::upon_error([](int) -> int { throw failure{3}; })),
                        3);
    }
    SECTION("upon_stopped") {
        check_exception(run(execution::just_stopped() |
                            execution::upon_stopped([]() -> int { throw failure{4}; })),
                        4);
    }
    SECTION("an unselected channel does not invoke the callable") {
        auto work =
            execution::just_error(7) | execution::then([](int) -> int { throw failure{5}; });
        STATIC_REQUIRE(std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                                    execution::completion_signatures<execution::set_error_t(int)>>);
        auto completion = run(std::move(work));
        REQUIRE(completion.errors == 1);
        REQUIRE(completion.error == 7);
        REQUIRE_FALSE(completion.exception);
    }
    SECTION("existing exception signatures are deduplicated") {
        auto work = execution::just_error(std::exception_ptr{}) |
                    execution::upon_error([](std::exception_ptr) -> int { throw failure{6}; });
        STATIC_REQUIRE(
            std::same_as<
                execution::completion_signatures_of_t<decltype(work)>,
                execution::completion_signatures<execution::set_value_t(int),
                                                 execution::set_error_t(std::exception_ptr)>>);
        check_exception(run(std::move(work)), 6);
    }
}

TEST_CASE("let_value catches input storage and callable failures",
          "[let_value][exceptions][lifecycle]") {
    int live = 0;
    const bool fail_copy = GENERATE(true, false);
    retained_input input{live, fail_copy};
    int calls = 0;
    auto work = execution::just(std::cref(input)) |
                execution::then([](std::reference_wrapper<const retained_input> value) noexcept
                                    -> const retained_input& { return value.get(); }) |
                execution::let_value([&](retained_input&) -> decltype(execution::just()) {
                    ++calls;
                    throw failure{2};
                });
    auto completion =
        run_and_destroy(std::move(work), [](auto& operation) { execution::start(operation); });
    check_exception(completion, fail_copy ? 1 : 2);
    REQUIRE(calls == (fail_copy ? 0 : 1));
    REQUIRE(live == 1);
}

TEST_CASE("let_value catches dependent connection failures after start",
          "[let_value][exceptions]") {
    int factories = 0, connections = 0, starts = 0;
    auto work = execution::just(1) | execution::let_value([&](int&) noexcept {
                    ++factories;
                    return connection_sender{&connections, &starts, true};
                });
    STATIC_REQUIRE(
        std::same_as<execution::completion_signatures_of_t<decltype(work)>,
                     execution::completion_signatures<execution::set_value_t(int),
                                                      execution::set_error_t(std::exception_ptr)>>);
    result completion;
    auto operation = execution::connect(std::move(work), result_receiver{&completion});
    REQUIRE(factories == 0);
    REQUIRE(connections == 0);
    execution::start(operation);
    check_exception(completion, 3);
    REQUIRE(factories == 1);
    REQUIRE(connections == 1);
    REQUIRE(starts == 0);
}

TEST_CASE("initial connection exceptions propagate to the caller", "[let_value][exceptions]") {
    int connections = 0, starts = 0;
    auto work = execution::let_value(connection_sender{&connections, &starts, true},
                                     [](int& value) noexcept { return execution::just(value); });
    result completion;
    REQUIRE_THROWS_AS(execution::connect(std::move(work), result_receiver{&completion}), failure);
    REQUIRE(connections == 1);
    REQUIRE(starts == 0);
    REQUIRE(completion.errors == 0);
    REQUIRE(completion.values == 0);
}

TEST_CASE("exception completions may destroy their operation", "[then][exceptions][lifecycle]") {
    auto completion = run_and_destroy(execution::just() | execution::then([] { throw failure{1}; }),
                                      [](auto& operation) { execution::start(operation); });
    check_exception(completion, 1);
}

TEST_CASE("repetition reports child connection failures",
          "[repeat][repeat_n][repeat_until][exceptions]") {
    const int fail_at = GENERATE(1, 3);
    int connections = 0, starts = 0;
    connection_sender source{&connections, &starts, true, fail_at};
    SECTION("repeat") { check_exception(run(execution::repeat(source)), 3); }
    SECTION("repeat_until") {
        check_exception(run(execution::repeat_until(source, [](int) noexcept { return false; })),
                        3);
    }
    SECTION("repeat_n with destruction on error") {
        auto work = execution::repeat_n(source, 3);
        STATIC_REQUIRE(
            std::same_as<
                execution::completion_signatures_of_t<decltype(work)>,
                execution::completion_signatures<execution::set_value_t(),
                                                 execution::set_error_t(std::exception_ptr)>>);
        check_exception(
            run_and_destroy(std::move(work), [](auto& operation) { execution::start(operation); }),
            3);
    }
    REQUIRE(connections == fail_at);
    REQUIRE(starts == fail_at - 1);
}
