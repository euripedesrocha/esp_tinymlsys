// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <concepts>
#include <memory>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution.hpp"
#include "esp_execution/detail/completion_signatures.hpp"

namespace {

using namespace esp::execution;

using execution_context_id = std::thread::id;

execution_context_id current_execution_context() noexcept { return std::this_thread::get_id(); }

struct move_counted {
    explicit move_counted(int* moves) : moves(moves) {}

    move_counted(const move_counted&) = delete;
    move_counted(move_counted&& other) noexcept : moves(other.moves) { *moves += 1; }

    int* moves;
};

struct probe {
    int values = 0;
    int arity = -1;
    int first = 0;
    int second = 0;
    int errors = 0;
    int error = 0;
    int stopped = 0;

    [[nodiscard]] int completions() const noexcept { return values + errors + stopped; }
};

struct probe_receiver {
    using receiver_concept = receiver_tag;

    probe* result;

    empty_env get_env() const noexcept { return {}; }

    void set_value() && noexcept { record_value(0); }

    void set_value(int value) && noexcept {
        record_value(1);
        result->first = value;
    }

    void set_value(int first, int second) && noexcept {
        record_value(2);
        result->first = first;
        result->second = second;
    }

    void set_value(std::unique_ptr<int>&& value) && noexcept {
        record_value(1);
        result->first = *value;
    }

    void set_value(move_counted&&) && noexcept { record_value(1); }

    void set_error(int value) && noexcept {
        result->errors += 1;
        result->error = value;
    }

    void set_stopped() && noexcept { result->stopped += 1; }

private:
    void record_value(int arity) noexcept {
        result->values += 1;
        result->arity = arity;
    }
};

struct thread_recording_receiver {
    using receiver_concept = receiver_tag;

    execution_context_id* completion_context;

    empty_env get_env() const noexcept { return {}; }

    void set_value(int) && noexcept { *completion_context = current_execution_context(); }
};

struct memberless_receiver {
    using receiver_concept = receiver_tag;
};

struct immovable_receiver {
    using receiver_concept = receiver_tag;

    immovable_receiver() = default;
    immovable_receiver(immovable_receiver&&) = delete;
};

struct invalid_completion_receiver {
    using receiver_concept = receiver_tag;

    void set_value(int) && {}
    int set_error(int) && noexcept { return 0; }
    void set_stopped() && {}
};

struct custom_env {
    int value;
};

struct throwing_env_receiver {
    using receiver_concept = receiver_tag;

    custom_env get_env() const { return {0}; }
};

struct custom_env_receiver : probe_receiver {
    custom_env_receiver(probe* result, int environment_value)
        : probe_receiver{result}, environment_value(environment_value) {}

    custom_env get_env() const noexcept { return {environment_value}; }

    int environment_value;
};

struct throwing_start_operation {
    using operation_state_concept = operation_state_tag;
    void start() &;
};

struct nonvoid_start_operation {
    using operation_state_concept = operation_state_tag;
    int start() & noexcept { return 0; }
};

struct untagged_startable {
    void start() & noexcept {}
};

struct simple_operation {
    using operation_state_concept = operation_state_tag;
    void start() & noexcept {}
};

struct immovable_sender {
    using sender_concept = sender_tag;

    immovable_sender() = default;
    immovable_sender(immovable_sender&&) = delete;
};

struct sender_without_signatures {
    using sender_concept = sender_tag;
};

struct minimal_sender {
    using sender_concept = sender_tag;

    template <class Env>
    using completion_signatures = esp::execution::completion_signatures<set_value_t()>;
};

struct malformed_signatures_sender {
    using sender_concept = sender_tag;

    template <class Env> using completion_signatures = esp::execution::completion_signatures<int>;
};

struct multi_signature_sender {
    using sender_concept = sender_tag;

    template <class Env>
    using completion_signatures =
        esp::execution::completion_signatures<set_value_t(int), set_value_t(int, int),
                                              set_error_t(int), set_stopped_t()>;
};

template <bool IsNoexcept> struct connect_sender {
    using sender_concept = sender_tag;

    template <class Env>
    using completion_signatures = esp::execution::completion_signatures<set_value_t()>;

    constexpr auto connect(receiver auto&&) && noexcept(IsNoexcept) { return simple_operation{}; }
};

struct nonoperation_connect_sender : minimal_sender {
    constexpr int connect(receiver auto&&) && noexcept { return 0; }
};

class environment_observing_sender {
    template <class Receiver> class operation {
    public:
        using operation_state_concept = operation_state_tag;

        explicit operation(Receiver receiver) : receiver_(std::move(receiver)) {}

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            const auto environment = esp::execution::get_env(receiver_);
            esp::execution::set_value(std::move(receiver_), environment.value);
        }

    private:
        Receiver receiver_;
    };

public:
    using sender_concept = sender_tag;

    explicit environment_observing_sender(int attribute) : attribute_(attribute) {}

    template <class Env>
    using completion_signatures = esp::execution::completion_signatures<set_value_t(int)>;

    custom_env get_env() const noexcept { return {attribute_}; }

    auto connect(receiver auto&& receiver) && {
        using receiver_type = std::decay_t<decltype(receiver)>;
        return operation<receiver_type>{std::forward<decltype(receiver)>(receiver)};
    }

private:
    int attribute_;
};

struct throwing_callable {
    int operator()(int value) const { return value; }
};

struct increment_callable {
    int operator()(int value) const noexcept { return value + 1; }
};

struct void_callable {
    void operator()(int) const noexcept {}
};

struct multi_signature_callable {
    long operator()(int value) const noexcept { return value; }
    int operator()(int first, int second) const noexcept { return first + second; }
};

struct uniform_signature_callable {
    int operator()(int value) const noexcept { return value; }
    int operator()(int first, int second) const noexcept { return first + second; }
};

struct noninvocable_callable {};

struct move_only_callable {
    explicit move_only_callable(std::unique_ptr<int> increment) : increment(std::move(increment)) {}

    int operator()(int value) const noexcept { return value + *increment; }

    std::unique_ptr<int> increment;
};

struct unmarked_adaptor {
    constexpr auto operator()(sender auto&& sender) const {
        return std::forward<decltype(sender)>(sender);
    }
};

struct nonsender_adaptor : sender_adaptor_closure<nonsender_adaptor> {
    constexpr int operator()(sender auto&&) const { return 0; }
};

struct copy_only_value {
    copy_only_value() = default;
    copy_only_value(const copy_only_value&) = default;
    copy_only_value(copy_only_value&&) = delete;
};

using one_sender = decltype(just(1));
using error_sender = decltype(just_error(17));
using stopped_sender = decltype(just_stopped());
using move_only_sender = decltype(just(std::make_unique<int>(1)));
using one_operation = decltype(connect(std::declval<one_sender>(), probe_receiver{nullptr}));
using copyable_then_closure = decltype(then(increment_callable{}));
using move_only_then_closure = decltype(then(std::declval<move_only_callable>()));
using move_only_then_sender =
    decltype(then(std::declval<one_sender>(), std::declval<move_only_callable>()));

template <class Sender, class Callable>
concept can_then = requires(Sender&& sender, Callable&& callable) {
    then(std::forward<Sender>(sender), std::forward<Callable>(callable));
};

template <class... Values>
concept can_just = requires(Values&&... values) { just(std::forward<Values>(values)...); };

template <class Operation>
concept can_start = requires(Operation& operation) { start(operation); };

template <class Sender, class Receiver>
concept can_connect = requires(Sender&& sender, Receiver&& receiver) {
    { connect(std::forward<Sender>(sender), std::forward<Receiver>(receiver)) } -> operation_state;
};

template <class Sender, class Closure>
concept can_pipe = requires(Sender&& sender, Closure&& closure) {
    std::forward<Sender>(sender) | std::forward<Closure>(closure);
};

template <class Object>
concept can_get_env = requires(const Object& object) { get_env(object); };

template <class Receiver>
concept can_set_value =
    requires(Receiver&& receiver) { set_value(std::forward<Receiver>(receiver), 1); };

template <class Receiver>
concept can_set_error =
    requires(Receiver&& receiver) { set_error(std::forward<Receiver>(receiver), 1); };

template <class Receiver>
concept can_set_stopped =
    requires(Receiver&& receiver) { set_stopped(std::forward<Receiver>(receiver)); };

} // namespace

TEST_CASE("public vocabulary concepts distinguish each role", "[concepts]") {
    STATIC_REQUIRE(sender<one_sender>);
    STATIC_REQUIRE(sender<error_sender>);
    STATIC_REQUIRE(sender<stopped_sender>);
    STATIC_REQUIRE(sender<sender_without_signatures>);
    STATIC_REQUIRE_FALSE(sender_in<sender_without_signatures>);
    STATIC_REQUIRE(sender_in<minimal_sender>);
    STATIC_REQUIRE_FALSE(sender_in<malformed_signatures_sender>);
    STATIC_REQUIRE_FALSE(sender<immovable_sender>);

    STATIC_REQUIRE(receiver<probe_receiver>);
    STATIC_REQUIRE(receiver<memberless_receiver>);
    STATIC_REQUIRE_FALSE(receiver<immovable_receiver>);
    STATIC_REQUIRE_FALSE(receiver<throwing_env_receiver>);

    STATIC_REQUIRE(operation_state<one_operation>);
    STATIC_REQUIRE_FALSE(operation_state<throwing_start_operation>);
    STATIC_REQUIRE_FALSE(operation_state<nonvoid_start_operation>);
    STATIC_REQUIRE_FALSE(operation_state<untagged_startable>);
}

TEST_CASE("public customization operations enforce their contracts", "[concepts][cpo]") {
    STATIC_REQUIRE(can_get_env<probe_receiver>);
    STATIC_REQUIRE(can_get_env<memberless_receiver>);
    STATIC_REQUIRE_FALSE(can_get_env<throwing_env_receiver>);
    STATIC_REQUIRE(
        std::same_as<decltype(get_env(std::declval<const memberless_receiver&>())), empty_env>);
    STATIC_REQUIRE(std::same_as<env_of_t<probe_receiver>, empty_env>);
    STATIC_REQUIRE(std::same_as<env_of_t<custom_env_receiver>, custom_env>);

    STATIC_REQUIRE(can_set_value<probe_receiver>);
    STATIC_REQUIRE(can_set_error<probe_receiver>);
    STATIC_REQUIRE(can_set_stopped<probe_receiver>);
    STATIC_REQUIRE_FALSE(can_set_value<memberless_receiver>);
    STATIC_REQUIRE_FALSE(can_set_error<memberless_receiver>);
    STATIC_REQUIRE_FALSE(can_set_stopped<memberless_receiver>);
    STATIC_REQUIRE_FALSE(can_set_value<invalid_completion_receiver>);
    STATIC_REQUIRE_FALSE(can_set_error<invalid_completion_receiver>);
    STATIC_REQUIRE_FALSE(can_set_stopped<invalid_completion_receiver>);

    STATIC_REQUIRE(can_start<one_operation>);
    STATIC_REQUIRE_FALSE(can_start<throwing_start_operation>);
    STATIC_REQUIRE_FALSE(can_start<nonvoid_start_operation>);
    STATIC_REQUIRE_FALSE(can_start<untagged_startable>);

    using nothrow_sender = connect_sender<true>;
    using potentially_throwing_sender = connect_sender<false>;
    STATIC_REQUIRE(can_connect<nothrow_sender, probe_receiver>);
    STATIC_REQUIRE(can_connect<potentially_throwing_sender, probe_receiver>);
    STATIC_REQUIRE(
        noexcept(connect(std::declval<nothrow_sender>(), std::declval<probe_receiver>())));
    STATIC_REQUIRE_FALSE(noexcept(
        connect(std::declval<potentially_throwing_sender>(), std::declval<probe_receiver>())));
}

TEST_CASE("connect rejects invalid member connections without substitution errors",
          "[connect][concepts]") {
    STATIC_REQUIRE_FALSE(can_connect<minimal_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<nonoperation_connect_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<connect_sender<true>&, probe_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<connect_sender<true>, int>);
    STATIC_REQUIRE_FALSE(detail::is_nothrow_member_connect_v<minimal_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(
        detail::is_nothrow_member_connect_v<nonoperation_connect_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(
        detail::is_nothrow_member_connect_v<connect_sender<true>&, probe_receiver>);
    STATIC_REQUIRE_FALSE(detail::is_nothrow_member_connect_v<connect_sender<true>, int>);
}

TEST_CASE("completion signatures describe sender outcomes", "[signatures]") {
    using one_signatures = completion_signatures_of_t<one_sender>;
    using error_signatures = completion_signatures_of_t<error_sender>;
    using stopped_signatures = completion_signatures_of_t<stopped_sender>;

    STATIC_REQUIRE(std::same_as<one_signatures, completion_signatures<set_value_t(int)>>);
    STATIC_REQUIRE(std::same_as<error_signatures, completion_signatures<set_error_t(int)>>);
    STATIC_REQUIRE(std::same_as<stopped_signatures, completion_signatures<set_stopped_t()>>);
}

TEST_CASE("completion signature predicates check every signature", "[signatures][concepts]") {
    constexpr auto is_value = []<class Signature> {
        return detail::completion_signature_matches_tag_v<Signature, set_value_t>;
    };

    STATIC_REQUIRE(detail::all_completion_signatures_satisfy_v<completion_signatures<>, is_value>);
    STATIC_REQUIRE(detail::all_completion_signatures_satisfy_v<completion_signatures<set_value_t()>,
                                                               is_value>);
    STATIC_REQUIRE(detail::all_completion_signatures_satisfy_v<
                   completion_signatures<set_value_t(), set_value_t(int)>, is_value>);
    STATIC_REQUIRE_FALSE(detail::all_completion_signatures_satisfy_v<
                         completion_signatures<set_error_t(int), set_value_t()>, is_value>);
    STATIC_REQUIRE_FALSE(detail::all_completion_signatures_satisfy_v<
                         completion_signatures<set_value_t(), set_stopped_t()>, is_value>);
    STATIC_REQUIRE_FALSE(detail::all_completion_signatures_satisfy_v<int, is_value>);
}

TEST_CASE("selected completion predicates inspect only their channel's arguments",
          "[signatures][concepts]") {
    constexpr auto is_int = []<class Argument> { return std::same_as<Argument, int>; };
    using value_and_terminals =
        completion_signatures<set_value_t(int), set_error_t(long), set_stopped_t()>;

    STATIC_REQUIRE(detail::all_selected_completion_signatures_satisfy_v<value_and_terminals,
                                                                        set_value_t, is_int>);
    STATIC_REQUIRE_FALSE(detail::all_selected_completion_signatures_satisfy_v<value_and_terminals,
                                                                              set_error_t, is_int>);
    STATIC_REQUIRE_FALSE(
        detail::all_selected_completion_signatures_satisfy_v<
            completion_signatures<set_value_t(int), set_value_t(long)>, set_value_t, is_int>);
    STATIC_REQUIRE(detail::all_selected_completion_signatures_satisfy_v<
                   completion_signatures<set_error_t(int), set_value_t(), set_stopped_t()>,
                   set_error_t, is_int>);

    // is_int cannot be called without a template argument. These channels must
    // be skipped, not merely have their predicate results discarded.
    STATIC_REQUIRE(
        detail::all_selected_completion_signatures_satisfy_v<completion_signatures<set_stopped_t()>,
                                                             set_value_t, is_int>);
    STATIC_REQUIRE(detail::all_selected_completion_signatures_satisfy_v<completion_signatures<>,
                                                                        set_value_t, is_int>);
    STATIC_REQUIRE_FALSE(
        detail::all_selected_completion_signatures_satisfy_v<int, set_value_t, is_int>);

    constexpr auto no_arguments = []<class... Arguments> { return sizeof...(Arguments) == 0; };
    STATIC_REQUIRE(
        detail::all_selected_completion_signatures_satisfy_v<completion_signatures<set_stopped_t()>,
                                                             set_stopped_t, no_arguments>);
    STATIC_REQUIRE(
        detail::all_selected_completion_signatures_satisfy_v<completion_signatures<set_value_t()>,
                                                             set_value_t, no_arguments>);
    STATIC_REQUIRE_FALSE(
        detail::all_selected_completion_signatures_satisfy_v<value_and_terminals, set_value_t,
                                                             no_arguments>);

    constexpr auto borrowed_arguments = []<class... Arguments> {
        return std::same_as<std::tuple<Arguments...>, std::tuple<int&, const long&>>;
    };
    STATIC_REQUIRE(detail::all_selected_completion_signatures_satisfy_v<
                   completion_signatures<set_value_t(int&, const long&)>, set_value_t,
                   borrowed_arguments>);
    STATIC_REQUIRE_FALSE(detail::all_selected_completion_signatures_satisfy_v<
                         completion_signatures<set_value_t(const long&, int&)>, set_value_t,
                         borrowed_arguments>);
}

TEST_CASE("then transforms every advertised value signature", "[then][signatures]") {
    STATIC_REQUIRE_FALSE(std::same_as<then_t, upon_error_t>);
    STATIC_REQUIRE_FALSE(std::same_as<then_t, upon_stopped_t>);
    STATIC_REQUIRE_FALSE(std::same_as<upon_error_t, upon_stopped_t>);

    using value_then_sender = decltype(then(std::declval<one_sender>(), increment_callable{}));
    using void_then_sender = decltype(then(std::declval<one_sender>(), void_callable{}));
    using multi_then_sender =
        decltype(then(std::declval<multi_signature_sender>(), multi_signature_callable{}));
    using deduplicated_then_sender =
        decltype(then(std::declval<multi_signature_sender>(), uniform_signature_callable{}));
    using error_then_sender = decltype(then(std::declval<error_sender>(), noninvocable_callable{}));
    using stopped_then_sender =
        decltype(then(std::declval<stopped_sender>(), noninvocable_callable{}));

    STATIC_REQUIRE(std::same_as<completion_signatures_of_t<value_then_sender>,
                                completion_signatures<set_value_t(int)>>);
    STATIC_REQUIRE(std::same_as<completion_signatures_of_t<void_then_sender>,
                                completion_signatures<set_value_t()>>);
    STATIC_REQUIRE(std::same_as<completion_signatures_of_t<multi_then_sender>,
                                completion_signatures<set_value_t(long), set_value_t(int),
                                                      set_error_t(int), set_stopped_t()>>);
    STATIC_REQUIRE(
        std::same_as<completion_signatures_of_t<deduplicated_then_sender>,
                     completion_signatures<set_value_t(int), set_error_t(int), set_stopped_t()>>);
    STATIC_REQUIRE(std::same_as<completion_signatures_of_t<error_then_sender>,
                                completion_signatures<set_error_t(int)>>);
    STATIC_REQUIRE(std::same_as<completion_signatures_of_t<stopped_then_sender>,
                                completion_signatures<set_stopped_t()>>);
    STATIC_REQUIRE(can_then<multi_signature_sender, multi_signature_callable>);
    STATIC_REQUIRE_FALSE(can_then<multi_signature_sender, increment_callable>);
    STATIC_REQUIRE_FALSE(can_then<one_sender, throwing_callable>);
}

TEST_CASE("factory and adaptor constraints preserve ownership", "[concepts][adaptor]") {
    using int_array = int[2];

    STATIC_REQUIRE(can_just<int>);
    STATIC_REQUIRE(can_just<int&>);
    STATIC_REQUIRE(can_just<std::unique_ptr<int>>);
    STATIC_REQUIRE_FALSE(can_just<std::unique_ptr<int>&>);
    STATIC_REQUIRE_FALSE(can_just<copy_only_value&>);
    STATIC_REQUIRE_FALSE(can_just<int_array&>);

    STATIC_REQUIRE(
        std::derived_from<copyable_then_closure, sender_adaptor_closure<copyable_then_closure>>);
    STATIC_REQUIRE(can_pipe<one_sender, copyable_then_closure>);
    STATIC_REQUIRE(can_pipe<one_sender, copyable_then_closure&>);
    STATIC_REQUIRE(can_pipe<one_sender, move_only_then_closure>);
    STATIC_REQUIRE_FALSE(can_pipe<one_sender, move_only_then_closure&>);
    STATIC_REQUIRE_FALSE(std::copy_constructible<move_only_then_closure>);
    STATIC_REQUIRE_FALSE(can_pipe<one_sender, unmarked_adaptor>);
    STATIC_REQUIRE_FALSE(can_pipe<one_sender, nonsender_adaptor>);

    STATIC_REQUIRE(can_connect<const one_sender&, probe_receiver>);
    STATIC_REQUIRE(can_connect<move_only_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<move_only_sender&, probe_receiver>);
    STATIC_REQUIRE(can_connect<move_only_then_sender, probe_receiver>);
    STATIC_REQUIRE_FALSE(can_connect<move_only_then_sender&, probe_receiver>);
}

TEST_CASE("library operation states cannot be copied or moved", "[operation_state]") {
    STATIC_REQUIRE_FALSE(std::copy_constructible<one_operation>);
    STATIC_REQUIRE_FALSE(std::move_constructible<one_operation>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<one_operation>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<one_operation>);
}

TEST_CASE("connect creates an operation without starting it", "[lifecycle]") {
    probe result;
    auto sender = just(42);
    auto operation = connect(std::move(sender), probe_receiver{&result});

    REQUIRE(result.completions() == 0);

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.values == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("just sends zero, one, and multiple values", "[just]") {
    probe result;

    SECTION("zero values") {
        auto operation = connect(just(), probe_receiver{&result});
        start(operation);

        REQUIRE(result.completions() == 1);
        REQUIRE(result.values == 1);
        REQUIRE(result.arity == 0);
    }

    SECTION("one value") {
        auto operation = connect(just(7), probe_receiver{&result});
        start(operation);

        REQUIRE(result.completions() == 1);
        REQUIRE(result.values == 1);
        REQUIRE(result.arity == 1);
        REQUIRE(result.first == 7);
    }

    SECTION("multiple values") {
        auto operation = connect(just(7, 9), probe_receiver{&result});
        start(operation);

        REQUIRE(result.completions() == 1);
        REQUIRE(result.values == 1);
        REQUIRE(result.arity == 2);
        REQUIRE(result.first == 7);
        REQUIRE(result.second == 9);
    }
}

TEST_CASE("just_error sends one error on start", "[just_error]") {
    probe result;
    auto operation = connect(just_error(17), probe_receiver{&result});

    REQUIRE(result.completions() == 0);

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.errors == 1);
    REQUIRE(result.error == 17);
}

TEST_CASE("just_stopped sends stopped on start", "[just_stopped]") {
    probe result;
    auto operation = connect(just_stopped(), probe_receiver{&result});

    REQUIRE(result.completions() == 0);

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.stopped == 1);
}

TEST_CASE("just delivers move-only values as rvalues", "[just]") {
    probe result;
    auto operation = connect(just(std::make_unique<int>(31)), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.values == 1);
    REQUIRE(result.first == 31);
}

TEST_CASE("just moves storage once into the sender and operation state", "[just][lifecycle]") {
    probe result;
    int moves = 0;
    move_counted value{&moves};

    auto sender = just(std::move(value));
    REQUIRE(moves == 1);

    auto operation = connect(std::move(sender), probe_receiver{&result});
    REQUIRE(moves == 2);

    start(operation);

    REQUIRE(moves == 2);
    REQUIRE(result.completions() == 1);
}

TEST_CASE("a copyable just sender creates independent operation states", "[just][lifecycle]") {
    probe first_result;
    probe second_result;
    const auto sender = just(42);

    auto first = connect(sender, probe_receiver{&first_result});
    auto second = connect(sender, probe_receiver{&second_result});

    start(first);
    start(second);

    REQUIRE(first_result.completions() == 1);
    REQUIRE(first_result.first == 42);
    REQUIRE(second_result.completions() == 1);
    REQUIRE(second_result.first == 42);
}

TEST_CASE("then transforms a value and remains lazy", "[then]") {
    probe result;
    int callable_calls = 0;
    auto sender = then(just(40), [&callable_calls](int value) noexcept {
        callable_calls += 1;
        return value + 2;
    });

    REQUIRE(callable_calls == 0);
    auto operation = connect(std::move(sender), probe_receiver{&result});
    REQUIRE(callable_calls == 0);
    REQUIRE(result.completions() == 0);

    start(operation);

    REQUIRE(callable_calls == 1);
    REQUIRE(result.completions() == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("then maps void results to an empty value completion", "[then]") {
    probe result;
    int observed = 0;
    auto sender = then(just(5), [&observed](int value) noexcept { observed = value; });
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(observed == 5);
    REQUIRE(result.completions() == 1);
    REQUIRE(result.values == 1);
    REQUIRE(result.arity == 0);
}

TEST_CASE("then supports pipe syntax and multiple input values", "[then]") {
    probe result;
    auto sender =
        just(20, 22) | then([](int first, int second) noexcept { return first + second; });
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("a copyable then closure supports direct calls and repeated piping", "[then]") {
    probe first_result;
    probe second_result;
    const auto increment = then(increment_callable{});

    auto first_sender = increment(just(40));
    auto second_sender = just(41) | increment;
    auto first = connect(std::move(first_sender), probe_receiver{&first_result});
    auto second = connect(std::move(second_sender), probe_receiver{&second_result});

    start(first);
    start(second);

    REQUIRE(first_result.completions() == 1);
    REQUIRE(first_result.first == 41);
    REQUIRE(second_result.completions() == 1);
    REQUIRE(second_result.first == 42);
}

TEST_CASE("an rvalue then closure transfers a move-only callable", "[then]") {
    probe result;
    auto add = then(move_only_callable{std::make_unique<int>(2)});
    auto sender = just(40) | std::move(add);
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("then delivers move-only results as rvalues", "[then]") {
    probe result;
    auto sender =
        then(just(40), [](int value) noexcept { return std::make_unique<int>(value + 2); });
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("a copyable then sender creates independent operation states", "[then][lifecycle]") {
    probe first_result;
    probe second_result;
    int callable_calls = 0;
    const auto sender = then(just(40), [&callable_calls](int value) noexcept {
        callable_calls += 1;
        return value + 1;
    });

    auto first = connect(sender, probe_receiver{&first_result});
    auto second = connect(sender, probe_receiver{&second_result});

    start(first);
    start(second);

    REQUIRE(callable_calls == 2);
    REQUIRE(first_result.completions() == 1);
    REQUIRE(first_result.first == 41);
    REQUIRE(second_result.completions() == 1);
    REQUIRE(second_result.first == 41);
}

TEST_CASE("errors bypass then without requiring a value callable", "[then][error]") {
    probe result;
    auto sender = then(just_error(17), noninvocable_callable{});
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.errors == 1);
    REQUIRE(result.error == 17);
}

TEST_CASE("stopped bypasses then without requiring a value callable", "[then][stopped]") {
    probe result;
    auto sender = then(just_stopped(), noninvocable_callable{});
    auto operation = connect(std::move(sender), probe_receiver{&result});

    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.stopped == 1);
}

TEST_CASE("then forwards sender and receiver environments", "[then][environment]") {
    auto sender = then(environment_observing_sender{7}, increment_callable{});
    const auto sender_environment = get_env(sender);
    REQUIRE(sender_environment.value == 7);

    probe result;
    auto operation = connect(std::move(sender), custom_env_receiver{&result, 41});
    start(operation);

    REQUIRE(result.completions() == 1);
    REQUIRE(result.first == 42);
}

TEST_CASE("then completes synchronously on the thread calling start", "[then][lifecycle]") {
    execution_context_id starting_context;
    execution_context_id callable_context;
    execution_context_id completion_context;

    std::thread worker([&] {
        starting_context = current_execution_context();
        auto sender = just(3) | then([&callable_context](int value) noexcept {
                          callable_context = current_execution_context();
                          return value;
                      });
        auto operation = connect(std::move(sender), thread_recording_receiver{&completion_context});
        start(operation);
    });
    worker.join();

    REQUIRE(callable_context == starting_context);
    REQUIRE(completion_context == starting_context);
}
