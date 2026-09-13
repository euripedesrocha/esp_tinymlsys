// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <cstdint>
#include <type_traits>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "esp_execution/completion.hpp"
#include "esp_execution/concepts.hpp"
#include "esp_execution/operation_state.hpp"

namespace esp_execution_test {

enum class completion_kind : std::uint8_t { value, error, stopped };

// One pending completion at a time; the test chooses when the child finishes.
struct deferred_completion {
    void* operation = nullptr;
    void (*complete)(void*, completion_kind) noexcept = nullptr;
    int starts = 0;

    void deliver(completion_kind kind) {
        REQUIRE(operation != nullptr);
        REQUIRE(complete != nullptr);
        auto* pending = std::exchange(operation, nullptr);
        auto callback = std::exchange(complete, nullptr);
        callback(pending, kind);
    }
};

struct deferred_sender {
    using sender_concept = esp::execution::sender_tag;

    template <class Env>
    using completion_signatures =
        esp::execution::completion_signatures<esp::execution::set_value_t(),
                                              esp::execution::set_error_t(int),
                                              esp::execution::set_stopped_t()>;

    deferred_completion* pending;

    template <class Receiver> class operation {
    public:
        using operation_state_concept = esp::execution::operation_state_tag;

        operation(deferred_completion& pending, Receiver receiver)
            : pending_(&pending), receiver_(std::move(receiver)) {}

        operation(const operation&) = delete;
        operation(operation&&) = delete;
        operation& operator=(const operation&) = delete;
        operation& operator=(operation&&) = delete;

        void start() & noexcept {
            pending_->starts += 1;
            pending_->operation = this;
            pending_->complete = [](void* erased, completion_kind kind) noexcept {
                // Completion may destroy this child or the whole parent operation.
                auto receiver = std::move(static_cast<operation*>(erased)->receiver_);
                switch (kind) {
                case completion_kind::value:
                    esp::execution::set_value(std::move(receiver));
                    break;
                case completion_kind::error:
                    esp::execution::set_error(std::move(receiver), 42);
                    break;
                case completion_kind::stopped:
                    esp::execution::set_stopped(std::move(receiver));
                    break;
                }
            };
        }

    private:
        deferred_completion* pending_;
        Receiver receiver_;
    };

    auto connect(esp::execution::receiver auto&& receiver) const& {
        return operation<std::decay_t<decltype(receiver)>>{
            *pending, std::forward<decltype(receiver)>(receiver)};
    }
};

} // namespace esp_execution_test
