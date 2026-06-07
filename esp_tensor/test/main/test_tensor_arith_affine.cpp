#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <array>
#include <cstdint>
#include <stdexcept>

#include "esp_tensor_arith_affine.hpp"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_ops.hpp"
#include "esp_tensor_view.hpp"

using Catch::Matchers::Message;

using namespace esp::tensor;

TEST_CASE("affine add uses scale ratios relative to the output domain", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({2, -2});
    auto rhs = std::to_array<std::int8_t>({4, -4});
    auto out = std::to_array<std::int8_t>({0, 0});
    const auto shape = tensor_shape{2};

    add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.5F, .zero_point = 0}),
        tensor_view(rhs, shape, domain::affine<>{.scale = 0.25F, .zero_point = 0}),
        tensor_view(out, shape, domain::affine<>{.scale = 0.25F, .zero_point = 0}));

    REQUIRE(out[0] == 8);
    REQUIRE(out[1] == -8);
}

TEST_CASE("affine add applies zero points before rescaling int8 inputs", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({3, -1, 127, -128});
    auto rhs = std::to_array<std::int8_t>({6, 0, 127, -128});
    auto out = std::to_array<std::int8_t>({0, 0, 0, 0});
    const auto shape = tensor_shape{4};

    add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.5F, .zero_point = 3}),
        tensor_view(rhs, shape, domain::affine<>{.scale = 0.25F, .zero_point = -2}),
        tensor_view(out, shape, domain::affine<>{.scale = 0.25F, .zero_point = 5}));

    REQUIRE(out[0] == 13);
    REQUIRE(out[1] == -1);
    REQUIRE(out[2] == 127);
    REQUIRE(out[3] == -128);
}

TEST_CASE("esp-nn affine add support accepts only representable int8 input offsets", "[affine]") {
    auto plan = detail::esp_nn::add_plan{
        .lhs_multiplier = {.multiplier = 1, .shift = 0},
        .rhs_multiplier = {.multiplier = 1, .shift = 0},
        .lhs_zero_point = -127,
        .rhs_zero_point = 128,
        .out_zero_point = 0,
    };

    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_zero_point = -128;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_zero_point = -127;
    plan.rhs_zero_point = 129;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));
}

TEST_CASE("accelerated affine add requires a supported int8 plan", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({1});
    auto rhs = std::to_array<std::int8_t>({1});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{1};
    const auto unsupported_lhs_domain = domain::affine<>{.scale = 1.0F, .zero_point = -128};

    if constexpr (hal::active_target() == hal::target::esp32s3 ||
                  hal::active_target() == hal::target::esp32p4) {
        REQUIRE_THROWS_MATCHES(add(tensor_view(lhs, shape, unsupported_lhs_domain),
                                   tensor_view(rhs, shape, domain::affine<>{}),
                                   tensor_view(out, shape, domain::affine<>{})),
                               std::invalid_argument,
                               Message("esp-nn affine add requires a supported quantization plan"));
    } else {
        add(tensor_view(lhs, shape, unsupported_lhs_domain),
            tensor_view(rhs, shape, domain::affine<>{}),
            tensor_view(out, shape, domain::affine<>{}));

        REQUIRE(out[0] == 127);
    }
}

TEST_CASE("affine add requires positive finite scales", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({1});
    auto rhs = std::to_array<std::int8_t>({1});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{1};

    REQUIRE_THROWS_MATCHES(add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.0F}),
                               tensor_view(rhs, shape, domain::affine<>{}),
                               tensor_view(out, shape, domain::affine<>{})),
                           std::invalid_argument,
                           Message("affine input scale must be finite and positive"));

    REQUIRE_THROWS_MATCHES(add(tensor_view(lhs, shape, domain::affine<>{.scale = -1.0F}),
                               tensor_view(rhs, shape, domain::affine<>{}),
                               tensor_view(out, shape, domain::affine<>{})),
                           std::invalid_argument,
                           Message("affine input scale must be finite and positive"));

    REQUIRE_THROWS_MATCHES(add(tensor_view(lhs, shape, domain::affine<>{}),
                               tensor_view(rhs, shape, domain::affine<>{}),
                               tensor_view(out, shape, domain::affine<>{.scale = 0.0F})),
                           std::invalid_argument,
                           Message("affine output scale must be finite and positive"));
}

TEST_CASE("scalar affine add uses the direct affine formula", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({10, -10, 120});
    auto rhs = std::to_array<std::int8_t>({6, -6, 120});
    auto out = std::to_array<std::int8_t>({0, 0, 0});

    const auto shape = tensor_shape{3};

    detail::add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.5F, .zero_point = 2}),
                tensor_view(rhs, shape, domain::affine<>{.scale = 0.25F, .zero_point = -2}),
                tensor_view(out, shape, domain::affine<>{.scale = 0.25F, .zero_point = 5}));

    REQUIRE(out[0] == 29);
    REQUIRE(out[1] == -23);
    REQUIRE(out[2] == 127);
}

TEST_CASE("affine add saturates default int8 domains", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({120, -120, 10});
    auto rhs = std::to_array<std::int8_t>({20, -20, -3});
    auto out = std::to_array<std::int8_t>({0, 0, 0});
    const auto shape = tensor_shape{3};

    add(tensor_view(lhs, shape, domain::affine<>{}), tensor_view(rhs, shape, domain::affine<>{}),
        tensor_view(out, shape, domain::affine<>{}));

    REQUIRE(out[0] == 127);
    REQUIRE(out[1] == -128);
    REQUIRE(out[2] == 7);
}
