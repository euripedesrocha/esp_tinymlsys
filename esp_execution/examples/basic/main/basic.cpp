// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <exception>
#include <utility>

#include <contracts>
#include <cstdlib>

#include "esp_execution.hpp"
#include "esp_log.h"

void handle_contract_violation(const std::contracts::contract_violation& /* violation */) noexcept {
    std::abort();
}

namespace {

constexpr auto tag = "esp_execution";

struct teaching_receiver {
    using receiver_concept = esp::execution::receiver_tag;

    int* result;

    constexpr esp::execution::empty_env get_env() const noexcept { return {}; }

    void set_value(int value) && noexcept { *result = value; }

    void set_error(auto&& /*unused*/) && noexcept { std::terminate(); }

    void set_stopped() && noexcept { std::terminate(); }
};

} // namespace

extern "C" void app_main(void) {
    using namespace esp::execution;

    int result = 0;
    auto work = just(40) | then([](int value) noexcept { return value + 2; });

    // Keep the explicit receiver so all three completion channels remain
    // visible in the example.
    auto operation = connect(std::move(work), teaching_receiver{&result});
    start(operation);

    ESP_LOGI(tag, "just(40) | then(...) completed with %d", result);
}
