// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <atomic>
#include <concepts>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "esp_execution.hpp"
#include "esp_freertos_cpp_test/support.hpp"
#include "esp_execution/freertos/task_context.hpp"
#include "esp_execution/freertos/wait.hpp"
#include "esp_execution_test/freertos.hpp"

namespace {
namespace execution = esp::execution;
using esp_execution_test::wait_support::deferred_sender;
using esp_execution_test::wait_support::pending_completion;
using esp_execution_test::wait_support::wait_until_blocked;
using esp_freertos_cpp_test::join_suspended_task;
using esp_freertos_cpp_test::release;
using esp_freertos_cpp_test::wait_for_completion;
using execution::freertos::wait;

template <class Sender>
concept waitable = requires(Sender&& sender) { wait(std::forward<Sender>(sender)); };

struct lifetime_record {
    int starts = 0;
    int destructions = 0;
};

// The explicit signatures also exercise reference decay and signature deduplication.
template <class Signatures, class Complete> struct inline_sender {
    using sender_concept = execution::sender_tag;
    template <class Env> using completion_signatures = Signatures;
    Complete complete;
    lifetime_record* lifetime = nullptr;

    template <class Receiver> struct operation {
        using operation_state_concept = execution::operation_state_tag;
        Receiver receiver;
        Complete complete;
        lifetime_record* lifetime;

        operation(Receiver receiver, Complete complete, lifetime_record* lifetime)
            : receiver(std::move(receiver)), complete(std::move(complete)), lifetime(lifetime) {}
        operation(const operation&) = delete;
        operation(operation&&) = delete;
        ~operation() {
            if (lifetime)
                ++lifetime->destructions;
        }
        void start() & noexcept {
            if (lifetime)
                ++lifetime->starts;
            complete(std::move(receiver));
        }
    };

    auto connect(execution::detail::receiver_for_sender<inline_sender> auto&& receiver) const {
        return operation<std::decay_t<decltype(receiver)>>{
            std::forward<decltype(receiver)>(receiver), complete, lifetime};
    }
};

template <class Signatures> auto source(auto complete, lifetime_record* lifetime = nullptr) {
    return inline_sender<Signatures, decltype(complete)>{complete, lifetime};
}

struct throwing_copy {
    throwing_copy(const throwing_copy&) noexcept(false);
    throwing_copy(throwing_copy&&) noexcept = default;
};

struct throwing_move {
    throwing_move(const throwing_move&) noexcept = default;
    throwing_move(throwing_move&&) noexcept(false);
};

} // namespace

TEST_CASE("FreeRTOS wait owns inline values and starts exactly once", "[freertos][wait]") {
    const auto values = wait(execution::just(42, 7L));
    STATIC_REQUIRE(std::same_as<decltype(values), const std::optional<std::tuple<int, long>>>);
    REQUIRE(values.has_value());
    REQUIRE(*values == std::tuple{42, 7L});

    const auto empty = wait(execution::just());
    STATIC_REQUIRE(std::same_as<decltype(empty), const std::optional<std::tuple<>>>);
    REQUIRE(empty.has_value());
    REQUIRE_FALSE(wait(execution::just_stopped()).has_value());

    lifetime_record record;
    auto sender = source<execution::completion_signatures<execution::set_value_t(int)>>(
        [&record](auto receiver) noexcept {
            contract_assert(record.destructions == 0);
            execution::set_value(std::move(receiver), 42);
        },
        &record);
    const auto recorded = wait(sender);
    REQUIRE(recorded.has_value());
    REQUIRE(std::get<0>(*recorded) == 42);
    REQUIRE(record.starts == 1);
    REQUIRE(record.destructions == 1);
}

TEST_CASE("FreeRTOS wait decays references and preserves move-only values", "[freertos][wait]") {
    int original = 42;
    auto sender = source<execution::completion_signatures<execution::set_value_t(int&),
                                                          execution::set_value_t(const int&)>>(
        [&original](auto receiver) noexcept {
            execution::set_value(std::move(receiver), original);
        });
    auto result = wait(sender);
    STATIC_REQUIRE(std::same_as<decltype(result), std::optional<std::tuple<int>>>);
    original = 7;
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);

    auto pointer = std::make_unique<int>(17);
    auto moved = wait(execution::just(std::move(pointer)));
    REQUIRE(pointer == nullptr);
    REQUIRE(moved.has_value());
    REQUIRE(std::get<0>(*moved) != nullptr);
    REQUIRE(*std::get<0>(*moved) == 17);
}

TEST_CASE("FreeRTOS wait rejects unhandled errors and unsafe result storage",
          "[freertos][wait][constraints]") {
    STATIC_REQUIRE_FALSE(waitable<decltype(execution::just_error(7))>);
    const auto incompatible = source<execution::completion_signatures<
        execution::set_value_t(int), execution::set_value_t(long)>>([](auto) noexcept {});
    STATIC_REQUIRE_FALSE(waitable<decltype(incompatible)>);
    const auto throwing =
        source<execution::completion_signatures<execution::set_value_t(throwing_copy&)>>(
            [](auto) noexcept {});
    STATIC_REQUIRE_FALSE(waitable<decltype(throwing)>);

    // Storage from an lvalue is safe, but returning the result would move it.
    STATIC_REQUIRE(std::is_nothrow_constructible_v<throwing_move, throwing_move&>);
    STATIC_REQUIRE_FALSE(std::is_nothrow_move_constructible_v<throwing_move>);
    const auto throwing_on_return =
        source<execution::completion_signatures<execution::set_value_t(throwing_move&)>>(
            [](auto) noexcept {});
    STATIC_REQUIRE_FALSE(waitable<decltype(throwing_on_return)>);

    auto recovered = execution::just_error(7) |
                     execution::upon_error([](int error) noexcept { return error + 1; });
    STATIC_REQUIRE(waitable<decltype(recovered)>);
    const auto result = wait(recovered);
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 8);
}

TEST_CASE("FreeRTOS wait accepts task completion without releasing its operation early",
          "[freertos][wait][lifetime]") {
    pending_completion pending;
    bool was_alive = false;
    SECTION("value") { pending.stop = false; }
    SECTION("stopped") { pending.stop = true; }
    struct producer_state {
        pending_completion& pending;
        bool& was_alive;
    } state{pending, was_alive};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<producer_state*>(argument);
            wait_for_completion(state.pending.ready);
            wait_until_blocked(state.pending.waiter);
            state.was_alive = !state.pending.destroyed.load();
            if (state.was_alive)
                state.pending.complete(state.pending.operation);
            vTaskSuspend(nullptr);
        },
        "wait_producer", 4096, &state, tskIDLE_PRIORITY + 2, &producer);
    REQUIRE(created == pdPASS);
    const auto result = wait(deferred_sender{pending});
    join_suspended_task(producer);
    REQUIRE(was_alive);
    REQUIRE(pending.destroyed.load());
    REQUIRE(result.has_value() == !pending.stop);
    if (result)
        REQUIRE(std::get<0>(*result) == 42);
}

#if INCLUDE_xTaskAbortDelay
TEST_CASE("FreeRTOS wait retries an aborted semaphore acquisition", "[freertos][wait][lifetime]") {
    pending_completion pending;
    bool abort_succeeded = false;
    bool stayed_alive = false;
    struct producer_state {
        pending_completion& pending;
        bool& abort_succeeded;
        bool& stayed_alive;
    } state{pending, abort_succeeded, stayed_alive};
    TaskHandle_t producer = nullptr;
    const auto created = xTaskCreate(
        [](void* argument) {
            auto& state = *static_cast<producer_state*>(argument);
            wait_for_completion(state.pending.ready);
            wait_until_blocked(state.pending.waiter);
            state.abort_succeeded = xTaskAbortDelay(state.pending.waiter) == pdPASS;
            // Let the waiter run, observe the aborted acquire, and block again.
            vTaskDelay(TickType_t{2});
            wait_until_blocked(state.pending.waiter);
            state.stayed_alive = !state.pending.destroyed.load();
            if (state.stayed_alive)
                state.pending.complete(state.pending.operation);
            vTaskSuspend(nullptr);
        },
        "wait_abort", 4096, &state, tskIDLE_PRIORITY + 2, &producer);
    REQUIRE(created == pdPASS);
    const auto result = wait(deferred_sender{pending});
    join_suspended_task(producer);
    REQUIRE(abort_succeeded);
    REQUIRE(stayed_alive);
    REQUIRE(pending.destroyed.load());
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);
}
#endif

TEST_CASE("FreeRTOS wait works with recovered task-context scheduling",
          "[freertos][wait][integration]") {
    execution::freertos::task_context worker{{.name = "wait_worker", .priority = 3}};
    std::atomic_bool correct_task = false;
    const auto owner = xTaskGetCurrentTaskHandle();
    auto work =
        execution::schedule(worker.get_scheduler()) | execution::then([&]() noexcept {
            correct_task.store(xTaskGetCurrentTaskHandle() != owner && !xPortInIsrContext());
            return 42;
        }) |
        execution::upon_error([](auto) noexcept { return -1; });
    const auto result = wait(work);
    REQUIRE(result.has_value());
    REQUIRE(std::get<0>(*result) == 42);
    REQUIRE(correct_task.load());
}

#if CONFIG_IDF_TARGET_ESP32S3 && !CONFIG_FREERTOS_UNICORE
TEST_CASE("FreeRTOS wait can destroy its state while task completion returns on the other core",
          "[freertos][wait][lifetime][multicore]") {
    const bool stopped = GENERATE(false, true);
    CAPTURE(stopped);
    for (unsigned iteration = 0; iteration < 32; ++iteration) {
        CAPTURE(iteration);
        const auto result = esp_execution_test::wait_support::complete_on_other_core(
            stopped, [](const auto& complete) { complete(); });
        REQUIRE(result.waiter_core == 0);
        REQUIRE(result.completion_core == 1);
        REQUIRE_FALSE(result.completion_from_isr);
        REQUIRE(result.value_correct);
        REQUIRE(result.returned);
        REQUIRE(result.destroyed);
    }
}
#endif
