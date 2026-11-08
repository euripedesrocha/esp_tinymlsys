// SPDX-License-Identifier: Apache-2.0
#include "esp_execution/just.hpp"
#include "esp_execution/let_value.hpp"
#include "esp_execution/repeat.hpp"
#include "esp_execution/then.hpp"
#include "esp_execution/upon_error.hpp"
#include "esp_execution/upon_stopped.hpp"
#include <catch2/generators/catch_generators.hpp>
#include <catch2/catch_test_macros.hpp>

namespace {
namespace ex = esp::execution;
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
