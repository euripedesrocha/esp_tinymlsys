// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/starts_on.hpp"
#include "esp_execution/just.hpp"
#include "esp_execution/then.hpp"
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include "esp_execution_test/execution.hpp"
#include <system_error>

namespace {
namespace ex = esp::execution;

enum class scheduling_result { value, error, stopped };

struct test_scheduler {
    using scheduler_concept = ex::scheduler_tag;
    int* calls;
    bool* executing;
    scheduling_result completion = scheduling_result::value;
    struct source {
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            Receiver receiver;
            int* calls;
            bool* executing;
            scheduling_result completion;
            void start() & noexcept {
                ++*calls;
                if (completion == scheduling_result::error) {
                    ex::set_error(std::move(receiver),
                                  std::make_error_code(std::errc::resource_unavailable_try_again));
                    return;
                }
                if (completion == scheduling_result::stopped) {
                    ex::set_stopped(std::move(receiver));
                    return;
                }
                auto* agent = executing;
                *agent = true;
                ex::set_value(std::move(receiver));
                *agent = false;
            }
        };
        using sender_concept = ex::sender_tag;
        int* calls;
        bool* executing;
        scheduling_result completion;
        struct environment {
            int* calls;
            bool* executing;
            scheduling_result completion;
            auto query(ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
                return test_scheduler{calls, executing, completion};
            }
        };
        auto get_env() const noexcept { return environment{calls, executing, completion}; }
        template <class Env>
        using completion_signatures =
            ex::completion_signatures<ex::set_value_t(), ex::set_error_t(std::error_code),
                                      ex::set_stopped_t()>;
        auto connect(ex::receiver auto receiver) const noexcept {
            return operation<decltype(receiver)>{std::move(receiver), calls, executing, completion};
        }
    };
    auto schedule() const noexcept { return source{calls, executing, completion}; }
    bool operator==(const test_scheduler&) const = default;
};

struct context_key {};
struct receiver_environment {
    int query(context_key) const noexcept { return 42; }
};
struct child_state {
    int connections = 0, starts = 0;
    bool fail_connect = false;
    test_scheduler scheduler;
};
struct child_sender {
    using sender_concept = ex::sender_tag;
    child_state* state;
    template <class Env>
    using completion_signatures = ex::completion_signatures<ex::set_value_t(int)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::receiver auto receiver) const {
        ++state->connections;
        CHECK(*state->scheduler.executing);
        auto env = ex::get_env(receiver);
        CHECK(ex::get_scheduler(env) == state->scheduler);
        CHECK(env.query(context_key{}) == 42);
        if (state->fail_connect)
            throw 43;
        return ex::connect(ex::just() | ex::then([state = state]() noexcept {
                               CHECK(*state->scheduler.executing);
                               ++state->starts;
                               return 7;
                           }),
                           std::move(receiver));
    }
};

struct attributed_child_sender {
    using sender_concept = ex::sender_tag;
    child_state* state;
    template <class Env>
    using completion_signatures =
        ex::completion_signatures<ex::set_value_t(int), ex::set_error_t(std::error_code),
                                  ex::set_stopped_t()>;
    struct environment {
        test_scheduler scheduler;
        template <class Tag> auto query(ex::get_completion_scheduler_t<Tag>) const noexcept {
            return scheduler;
        }
        int query(context_key) const noexcept { return 42; }
    };
    auto get_env() const noexcept { return environment{state->scheduler}; }
    auto connect(ex::receiver auto receiver) const {
        ++state->connections;
        auto work = ex::schedule(state->scheduler) | ex::then([state = state]() noexcept {
                        ++state->starts;
                        return 7;
                    });
        return ex::connect(std::move(work), std::move(receiver));
    }
};
struct result {
    int values = 0, errors = 0, exceptions = 0, stopped = 0;
    int value = 0, exception_value = 0;
    std::error_code error;
};
struct receiver {
    using receiver_concept = ex::receiver_tag;
    result* output;
    auto get_env() const noexcept { return receiver_environment{}; }
    void set_value(int value) && noexcept {
        ++output->values;
        output->value = value;
    }
    void set_error(std::error_code error) && noexcept {
        ++output->errors;
        output->error = error;
    }
    void set_error(std::exception_ptr error) && noexcept {
        ++output->exceptions;
        try {
            std::rethrow_exception(error);
        } catch (int value) {
            output->exception_value = value;
        } catch (...) {
            FAIL_CHECK("Unexpected exception in starts_on test");
        }
    }
    void set_stopped() && noexcept { ++output->stopped; }
};

struct reference_environment {
    reference_environment() = default;
    reference_environment(const reference_environment&) = delete;
    int query(context_key) const noexcept { return 42; }
};
struct reference_receiver : receiver {
    const reference_environment* environment;
    const reference_environment& get_env() const noexcept { return *environment; }
};

enum class selected_overload { none, lvalue, rvalue };
struct qualified_scheduler {
    using scheduler_concept = ex::scheduler_tag;
    selected_overload* selected;
    struct environment {
        selected_overload* selected;
        auto query(ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
            return qualified_scheduler{selected};
        }
    };
    template <selected_overload Selection> struct source {
        using sender_concept = ex::sender_tag;
        selected_overload* selected;
        template <class Env>
        using completion_signatures = ex::completion_signatures<ex::set_value_t()>;
        auto get_env() const noexcept { return environment{selected}; }
        template <class Receiver> struct operation {
            using operation_state_concept = ex::operation_state_tag;
            selected_overload* selected;
            Receiver receiver;
            void start() & noexcept {
                *selected = Selection;
                ex::set_value(std::move(receiver));
            }
        };
        auto connect(ex::receiver auto receiver) const noexcept {
            return operation<decltype(receiver)>{selected, std::move(receiver)};
        }
    };
    auto schedule() const& noexcept { return source<selected_overload::lvalue>{selected}; }
    auto schedule() && noexcept { return source<selected_overload::rvalue>{selected}; }
    bool operator==(const qualified_scheduler&) const = default;
};

struct receiver_dependent_sender {
    using sender_concept = ex::sender_tag;
    template <class Env>
    using completion_signatures = ex::completion_signatures<ex::set_value_t(int)>;
    auto get_env() const noexcept { return ex::empty_env{}; }
    auto connect(ex::receiver auto receiver) const noexcept(sizeof(decltype(receiver)) == 1) {
        if constexpr (sizeof(decltype(receiver)) != 1)
            throw 47;
        return ex::connect(ex::just(7), std::move(receiver));
    }
};

struct destroying_receiver : esp_execution_test::destroying_receiver {
    using base = esp_execution_test::destroying_receiver;
    using base::base;
    using base::set_error;
    using base::set_value;
    void set_value(int value) && noexcept {
        state->completion.value = value;
        std::move(static_cast<base&>(*this)).set_value();
    }
    void set_error(std::error_code error) && noexcept {
        std::move(static_cast<base&>(*this)).set_error(error.value());
    }
};

SCENARIO("starts_on schedules before connecting and starting its child", "[starts_on]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};
    child_state state{.scheduler = scheduler};
    result output;

    GIVEN("a scheduler that completes successfully") {
        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(scheduler, child_sender{&state});
            auto operation = ex::connect(std::move(sender), receiver{&output});
            CHECK((scheduled == 0 && state.connections == 0 && state.starts == 0));
            ex::start(operation);

            THEN("the child connects and starts on the scheduler's execution agent") {
                CHECK(scheduled == 1);
                CHECK((state.connections == 1 && state.starts == 1));
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK(!executing);
            }
        }
    }

    GIVEN("a scheduler that reports an error") {
        scheduler.completion = scheduling_result::error;

        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(scheduler, child_sender{&state});
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the scheduling error arrives without connecting the child") {
                CHECK(scheduled == 1);
                CHECK((state.connections == 0 && state.starts == 0));
                CHECK(output.errors == 1);
                CHECK(output.error == std::errc::resource_unavailable_try_again);
                CHECK((output.values == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK(!executing);
            }
        }
    }

    GIVEN("a scheduler that completes stopped") {
        scheduler.completion = scheduling_result::stopped;

        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(scheduler, child_sender{&state});
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the stopped completion arrives without connecting the child") {
                CHECK(scheduled == 1);
                CHECK((state.connections == 0 && state.starts == 0));
                CHECK(output.stopped == 1);
                CHECK((output.values == 0 && output.errors == 0 && output.exceptions == 0));
                CHECK(!executing);
            }
        }
    }
}

SCENARIO("starts_on reports delayed child connection exceptions", "[starts_on][exceptions]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};
    child_state state{.fail_connect = true, .scheduler = scheduler};
    result output;

    GIVEN("a child whose connection throws") {
        WHEN("scheduling succeeds and the child connection is attempted") {
            auto sender = ex::starts_on(scheduler, child_sender{&state});
            auto operation = ex::connect(std::move(sender), receiver{&output});
            CHECK(state.connections == 0);
            ex::start(operation);

            THEN("the original exception arrives without starting the child") {
                CHECK((scheduled == 1 && state.connections == 1 && state.starts == 0));
                CHECK((output.exceptions == 1 && output.exception_value == 43));
                CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
                CHECK(!executing);
            }
        }
    }
}

SCENARIO("starts_on does not promise its child's completion schedulers",
         "[starts_on][environment]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};
    int child_scheduled = 0;
    bool child_executing = false;
    child_state state{.scheduler = {&child_scheduled, &child_executing}};
    result output;

    GIVEN("a child advertising value, error, and stopped completion schedulers") {
        using child_env = ex::env_of_t<attributed_child_sender>;
        STATIC_REQUIRE(ex::detail::completion_scheduler_provider<child_env, ex::set_value_t>);
        STATIC_REQUIRE(ex::detail::completion_scheduler_provider<child_env, ex::set_error_t>);
        STATIC_REQUIRE(ex::detail::completion_scheduler_provider<child_env, ex::set_stopped_t>);

        const auto failure = GENERATE(scheduling_result::error, scheduling_result::stopped);
        scheduler.completion = failure;

        WHEN("initial scheduling fails before connecting the child") {
            auto sender = ex::starts_on(scheduler, attributed_child_sender{&state});
            using env = ex::env_of_t<decltype(sender)>;
            STATIC_REQUIRE_FALSE(ex::detail::completion_scheduler_provider<env, ex::set_value_t>);
            STATIC_REQUIRE_FALSE(ex::detail::completion_scheduler_provider<env, ex::set_error_t>);
            STATIC_REQUIRE_FALSE(ex::detail::completion_scheduler_provider<env, ex::set_stopped_t>);
            CHECK(ex::get_env(sender).query(context_key{}) == 42);

            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("failure completes without claiming the unused child's resource") {
                CHECK(scheduled == 1);
                CHECK(child_scheduled == 0);
                CHECK(state.connections == 0);
                CHECK(state.starts == 0);
                CHECK(output.values == 0);
                CHECK(output.exceptions == 0);
                CHECK(output.errors == (failure == scheduling_result::error ? 1 : 0));
                CHECK(output.stopped == (failure == scheduling_result::stopped ? 1 : 0));
            }
        }
    }
}

SCENARIO("starts_on forwards child terminal completions", "[starts_on]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};
    result output;
    const auto error = std::make_error_code(std::errc::io_error);

    GIVEN("a child that produces a value") {
        WHEN("the sender is started") {
            auto operation = ex::connect(ex::starts_on(scheduler, ex::just(7)), receiver{&output});
            ex::start(operation);

            THEN("one value reaches the receiver") {
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }
    }

    GIVEN("a child that produces an error") {
        WHEN("the sender is started") {
            auto operation =
                ex::connect(ex::starts_on(scheduler, ex::just_error(error)), receiver{&output});
            ex::start(operation);

            THEN("one error reaches the receiver") {
                CHECK((output.errors == 1 && output.error == error));
                CHECK((output.values == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }
    }

    GIVEN("a child that completes stopped") {
        WHEN("the sender is started") {
            auto operation =
                ex::connect(ex::starts_on(scheduler, ex::just_stopped()), receiver{&output});
            ex::start(operation);

            THEN("one stopped completion reaches the receiver") {
                CHECK(output.stopped == 1);
                CHECK((output.values == 0 && output.errors == 0 && output.exceptions == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }
    }
}

SCENARIO("starts_on preserves reference-valued environments", "[starts_on][environment]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};
    child_state state{.scheduler = scheduler};
    reference_environment environment;
    result output;

    GIVEN("a receiver exposing a noncopyable environment by const reference") {
        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(scheduler, child_sender{&state});
            auto operation =
                ex::connect(std::move(sender), reference_receiver{{&output}, &environment});
            ex::start(operation);

            THEN("the child can query the original environment and the associated scheduler") {
                CHECK((scheduled == 1 && state.connections == 1 && state.starts == 1));
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK(!executing);
            }
        }
    }

    GIVEN("a sender and receiver exposing nonmovable environments by rvalue reference") {
        struct source : child_sender {
            reference_environment* environment;
            reference_environment&& get_env() const noexcept { return std::move(*environment); }
        };
        struct rvalue_receiver : receiver {
            reference_environment* environment;
            reference_environment&& get_env() const noexcept { return std::move(*environment); }
        };

        WHEN("the scheduled sender's environment is queried and its operation is started") {
            auto sender = ex::starts_on(scheduler, source{{&state}, &environment});
            CHECK(ex::get_env(sender).query(context_key{}) == 42);
            auto operation =
                ex::connect(std::move(sender), rvalue_receiver{{&output}, &environment});
            ex::start(operation);

            THEN("both environment references remain usable without moving their referent") {
                CHECK((scheduled == 1 && state.connections == 1 && state.starts == 1));
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.exceptions == 0 && output.stopped == 0));
                CHECK(!executing);
            }
        }
    }
}

SCENARIO("starts_on consistently schedules with an rvalue scheduler", "[starts_on]") {
    selected_overload selected = selected_overload::none;
    qualified_scheduler scheduler{&selected};
    result output;

    GIVEN("a scheduler with distinct lvalue and rvalue schedule operation types") {
        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(scheduler, ex::just(7));
            STATIC_REQUIRE(
                std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                             ex::completion_signatures<ex::set_value_t(int),
                                                       ex::set_error_t(std::exception_ptr)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&output});
            CHECK(selected == selected_overload::none);
            ex::start(operation);

            THEN("the selected operation matches the advertised signatures and stored type") {
                CHECK(selected == selected_overload::rvalue);
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.exceptions == 0 && output.stopped == 0));
            }
        }
    }
}

SCENARIO("starts_on accounts for receiver-dependent connection exceptions",
         "[starts_on][exceptions]") {
    selected_overload selected = selected_overload::none;
    qualified_scheduler scheduler{&selected};
    result output;

    GIVEN("a child whose connection is nonthrowing only for empty receivers") {
        WHEN("the child is connected to the actual forwarding receiver") {
            auto sender = ex::starts_on(scheduler, receiver_dependent_sender{});
            STATIC_REQUIRE(
                std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                             ex::completion_signatures<ex::set_value_t(int),
                                                       ex::set_error_t(std::exception_ptr)>>);
            auto operation = ex::connect(std::move(sender), receiver{&output});
            ex::start(operation);

            THEN("the connection exception reaches the advertised error channel") {
                CHECK(selected == selected_overload::rvalue);
                CHECK((output.exceptions == 1 && output.exception_value == 47));
                CHECK((output.values == 0 && output.errors == 0 && output.stopped == 0));
            }
        }
    }
}

SCENARIO("starts_on permits destruction during terminal completion", "[starts_on][lifetime]") {
    int scheduled = 0;
    bool executing = false;
    test_scheduler scheduler{&scheduled, &executing};

    GIVEN("a receiver that destroys its operation during completion") {
        WHEN("a child value completes synchronously") {
            auto output = esp_execution_test::complete_and_destroy<destroying_receiver>(
                ex::starts_on(scheduler, ex::just(7)));

            THEN("the value survives destruction and scheduling returns safely") {
                CHECK((output.values == 1 && output.value == 7));
                CHECK((output.errors == 0 && output.stopped == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }

        WHEN("a child error completes synchronously") {
            auto output = esp_execution_test::complete_and_destroy<destroying_receiver>(
                ex::starts_on(scheduler, ex::just_error(9)));

            THEN("the error survives destruction and scheduling returns safely") {
                CHECK((output.errors == 1 && output.error == 9));
                CHECK((output.values == 0 && output.stopped == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }

        WHEN("the child completes stopped") {
            auto output = esp_execution_test::complete_and_destroy<destroying_receiver>(
                ex::starts_on(scheduler, ex::just_stopped()));

            THEN("the stopped completion destroys the operation safely") {
                CHECK(output.stopped == 1);
                CHECK((output.values == 0 && output.errors == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }

        WHEN("the scheduler reports an error") {
            scheduler.completion = scheduling_result::error;
            auto output = esp_execution_test::complete_and_destroy<destroying_receiver>(
                ex::starts_on(scheduler, ex::just(7)));

            THEN("the operation can be destroyed before its child is connected") {
                CHECK(output.errors == 1);
                CHECK(output.error ==
                      std::make_error_code(std::errc::resource_unavailable_try_again).value());
                CHECK((output.values == 0 && output.stopped == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }

        WHEN("the scheduler completes stopped") {
            scheduler.completion = scheduling_result::stopped;
            auto output = esp_execution_test::complete_and_destroy<destroying_receiver>(
                ex::starts_on(scheduler, ex::just(7)));

            THEN("the operation is destroyed without starting its child") {
                CHECK(output.stopped == 1);
                CHECK((output.values == 0 && output.errors == 0));
                CHECK((scheduled == 1 && !executing));
            }
        }
    }
}
} // namespace
