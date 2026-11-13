// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/starts_on.hpp"
#include "esp_execution/then.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
namespace ex = esp::execution;

struct immediate_scheduler {
    using scheduler_concept = ex::scheduler_tag;
    struct environment {
        auto query(ex::get_completion_scheduler_t<ex::set_value_t>) const noexcept {
            return immediate_scheduler{};
        }
    };
    using just_sender = decltype(ex::just());
    struct source : just_sender {
        auto get_env() const noexcept { return environment{}; }
    };
    auto schedule() const noexcept { return source{ex::just()}; }
    bool operator==(const immediate_scheduler&) const = default;
};
} // namespace

TEST_CASE("nonthrowing continuations execute with exception support disabled", "[no_exceptions]") {
    using Catch::Generators::table;
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        int* result;
        void set_value(int n) && noexcept { *result = n; }
    };
    static constexpr auto run = [](ex::sender auto source) noexcept {
        // A fully nonthrowing path needs neither an exception error signature nor
        // an exception handler on its receiver.
        STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(source)>,
                                    ex::completion_signatures<ex::set_value_t(int)>>);
        STATIC_REQUIRE(noexcept(ex::connect(std::move(source), receiver{})));
        int result = 0;
        auto operation = ex::connect(std::move(source), receiver{&result});
        ex::start(operation);
        return result;
    };
    const auto [name, run_case] = GENERATE(table<const char*, int (*)() noexcept>({
        {"then and let_value",
         []() noexcept {
             return run(ex::just(40) | ex::then([](int n) noexcept { return n + 1; }) |
                        ex::let_value([](int& n) noexcept { return ex::just(n + 1); }));
         }},
        {"upon_error",
         []() noexcept {
             return run(ex::just_error(7) | ex::upon_error([](int n) noexcept { return n + 35; }));
         }},
        {"upon_stopped",
         []() noexcept {
             return run(ex::just_stopped() | ex::upon_stopped([]() noexcept { return 42; }));
         }},
        {"let_value error recovery",
         []() noexcept {
             return run(ex::just() | ex::let_value([]() noexcept { return ex::just_error(7); }) |
                        ex::upon_error([](int n) noexcept { return n + 35; }));
         }},
    }));
    CAPTURE(name);
    CHECK(run_case() == 42);
}

TEST_CASE("nonthrowing repetition executes with exception support disabled",
          "[no_exceptions][repeat]") {
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        int* completions;
        void set_value() && noexcept { ++*completions; }
    };
    int iterations = 0, completions = 0;
    auto work = ex::repeat_n(ex::just() | ex::then([&]() noexcept { ++iterations; }), 3);
    STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(work)>,
                                ex::completion_signatures<ex::set_value_t()>>);
    STATIC_REQUIRE(noexcept(ex::connect(std::move(work), receiver{})));
    auto operation = ex::connect(std::move(work), receiver{&completions});
    ex::start(operation);
    CHECK(iterations == 3);
    CHECK(completions == 1);
}

TEST_CASE("nonthrowing dependent operations preserve reference environments",
          "[no_exceptions][environment]") {
    struct environment {
        environment() = default;
        environment(const environment&) = delete;
    };
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        environment* env;
        int* completions;
        const environment& get_env() const noexcept { return *env; }
        void set_value() && noexcept { ++*completions; }
    };
    environment env;
    int completions = 0;
    auto source = ex::just() |
                  ex::let_value([]() noexcept { return ex::just() | ex::then([]() noexcept {}); });
    auto work = ex::repeat_n(std::move(source), 2);
    STATIC_REQUIRE(std::same_as<ex::completion_signatures_of_t<decltype(work), const environment&>,
                                ex::completion_signatures<ex::set_value_t()>>);
    STATIC_REQUIRE(noexcept(ex::connect(std::move(work), receiver{})));
    auto operation = ex::connect(std::move(work), receiver{&env, &completions});
    ex::start(operation);
    CHECK(completions == 1);
}

SCENARIO("starts_on executes a nonthrowing child with exceptions disabled",
         "[starts_on][no_exceptions]") {
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        int* value;
        void set_value(int incoming) && noexcept { *value = incoming; }
        void set_error(std::exception_ptr) && noexcept { FAIL_CHECK("Unexpected exception"); }
    };
    int value = 0;

    GIVEN("a nonthrowing scheduler and a child producing an integer") {
        WHEN("the sender is connected and started") {
            auto sender = ex::starts_on(immediate_scheduler{}, ex::just(42));
            STATIC_REQUIRE(
                std::same_as<ex::completion_signatures_of_t<decltype(sender)>,
                             ex::completion_signatures<ex::set_value_t(int),
                                                       ex::set_error_t(std::exception_ptr)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&value});
            ex::start(operation);

            THEN("the nonthrowing execution path delivers the value") { CHECK(value == 42); }
        }
    }
}

SCENARIO("starts_on preserves reference environments with exceptions disabled",
         "[starts_on][no_exceptions][environment]") {
    struct environment {
        environment() = default;
        environment(const environment&) = delete;
    };
    struct receiver {
        using receiver_concept = ex::receiver_tag;
        const environment* env;
        int* completions;
        const environment& get_env() const noexcept { return *env; }
        void set_value() && noexcept { ++*completions; }
        void set_error(std::exception_ptr) && noexcept { FAIL_CHECK("Unexpected exception"); }
    };
    environment env;
    int completions = 0;

    GIVEN("a receiver exposing a noncopyable environment by reference") {
        WHEN("a nonthrowing scheduled sender is connected and started") {
            auto sender = ex::starts_on(immediate_scheduler{}, ex::just());
            STATIC_REQUIRE(
                std::same_as<ex::completion_signatures_of_t<decltype(sender), const environment&>,
                             ex::completion_signatures<ex::set_value_t(),
                                                       ex::set_error_t(std::exception_ptr)>>);
            STATIC_REQUIRE(noexcept(ex::connect(std::move(sender), receiver{})));
            auto operation = ex::connect(std::move(sender), receiver{&env, &completions});
            ex::start(operation);

            THEN("the operation completes without copying the environment") {
                CHECK(completions == 1);
            }
        }
    }
}
