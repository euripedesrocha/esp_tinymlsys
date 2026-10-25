// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#include <chrono>
#include <cstdint>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "esp_execution/freertos/chrono.hpp"

namespace {

using esp::execution::freertos::tick_duration;
using esp::execution::freertos::to_ticks;
using namespace std::chrono_literals;

} // namespace

TEST_CASE("nonpositive chrono timeouts convert to zero FreeRTOS ticks", "[freertos][chrono]") {
    const auto timeout =
        GENERATE(std::chrono::nanoseconds::min(), std::chrono::nanoseconds{-1s}, -1ns, 0ns);
    REQUIRE(to_ticks(timeout) == 0);
}

TEST_CASE("positive chrono timeouts round up to FreeRTOS ticks", "[freertos][chrono]") {
    const auto timeout =
        GENERATE(1ns, std::chrono::nanoseconds{1ms}, std::chrono::nanoseconds{15ms});
    // Independent integer ceiling calculation for sub-tick and fractional-tick inputs.
    const auto expected = (timeout.count() * configTICK_RATE_HZ + 999'999'999) / 1'000'000'000;
    REQUIRE(to_ticks(timeout) == expected);
}

TEST_CASE("chrono conversions preserve exact FreeRTOS tick boundaries", "[freertos][chrono]") {
    const auto ticks = GENERATE(1, 2, 7);
    const auto timeout = std::chrono::duration_cast<std::chrono::nanoseconds>(tick_duration{ticks});
    REQUIRE(to_ticks(timeout) == ticks);
    REQUIRE(to_ticks(timeout + 1ns) == ticks + 1);
}

TEST_CASE("coarser chrono units convert to FreeRTOS ticks at compile time", "[freertos][chrono]") {
    constexpr auto from_milliseconds = to_ticks(1000ms);
    constexpr auto from_seconds = to_ticks(1s);
    constexpr auto from_minutes = to_ticks(1min);
    REQUIRE(from_milliseconds == configTICK_RATE_HZ);
    REQUIRE(from_seconds == configTICK_RATE_HZ);
    REQUIRE(from_minutes == 60 * configTICK_RATE_HZ);
}

#if CONFIG_IDF_TARGET_ESP32S3
TEST_CASE("chrono conversion preserves the largest finite ESP32-S3 timeout", "[freertos][chrono]") {
    constexpr auto ticks = static_cast<std::int64_t>(portMAX_DELAY) - 1;
    constexpr auto timeout =
        std::chrono::duration_cast<std::chrono::nanoseconds>(tick_duration{ticks});
    constexpr auto at_boundary = to_ticks(timeout);
    constexpr auto just_before = to_ticks(timeout - 1ns);
    REQUIRE(at_boundary == portMAX_DELAY - 1);
    REQUIRE(just_before == portMAX_DELAY - 1);
}
#elif CONFIG_IDF_TARGET_LINUX
TEST_CASE("maximum nanoseconds fit the host FreeRTOS tick counter", "[freertos][chrono]") {
    constexpr auto timeout = std::chrono::nanoseconds::max();
    constexpr auto ticks = to_ticks(timeout);
    // Split seconds and remainder to avoid overflow in the independent calculation.
    constexpr auto seconds = timeout.count() / 1'000'000'000;
    constexpr auto remainder = timeout.count() % 1'000'000'000;
    constexpr auto expected = seconds * configTICK_RATE_HZ +
                              (remainder * configTICK_RATE_HZ + 999'999'999) / 1'000'000'000;
    REQUIRE(ticks == expected);
}
#endif
