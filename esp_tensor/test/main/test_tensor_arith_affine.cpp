#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>

#include "esp_tensor_ops_affine.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;

TEST_CASE("affine add uses scale ratios relative to the output domain", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({2, -2});
    auto rhs = std::to_array<std::int8_t>({4, -4});
    auto out = std::to_array<std::int8_t>({0, 0});
    const auto shape = tensor_shape{2};

    arith::add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.5F, .zero_point = 0}),
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

    arith::add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.5F, .zero_point = 3}),
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

    arith::add(tensor_view(lhs, shape, domain::affine<>{}),
               tensor_view(rhs, shape, domain::affine<>{}),
               tensor_view(out, shape, domain::affine<>{}));

    REQUIRE(out[0] == 127);
    REQUIRE(out[1] == -128);
    REQUIRE(out[2] == 7);
}

TEST_CASE("affine add with matching scales applies zero points", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({10, -10});
    auto rhs = std::to_array<std::int8_t>({6, -6});
    auto out = std::to_array<std::int8_t>({0, 0});
    const auto shape = tensor_shape{2};

    arith::add(tensor_view(lhs, shape, domain::affine<>{.scale = 0.25F, .zero_point = 2}),
               tensor_view(rhs, shape, domain::affine<>{.scale = 0.25F, .zero_point = -2}),
               tensor_view(out, shape, domain::affine<>{.scale = 0.25F, .zero_point = 5}));

    REQUIRE(out[0] == 21);
    REQUIRE(out[1] == -11);
}

TEST_CASE("esp-nn add plan support rejects left shifts outside int32 range", "[affine]") {
    auto plan = detail::esp_nn::add_plan{
        .lhs_multiplier = {.multiplier = 1, .shift = 30},
        .rhs_multiplier = {.multiplier = 1, .shift = 0},
        .lhs_zero_point = 0,
        .rhs_zero_point = 0,
        .out_zero_point = 0,
    };

    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_multiplier.shift = 31;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_multiplier.shift = 0;
    plan.rhs_multiplier.shift = 30;
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.rhs_multiplier.shift = 31;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));
}

TEST_CASE("esp-nn add plan support normalizes positive multiplier shifts", "[affine]") {
    auto plan = detail::esp_nn::add_plan{
        .lhs_multiplier = {.multiplier = 1, .shift = 2},
        .rhs_multiplier = {.multiplier = 1, .shift = 0},
        .lhs_zero_point = 0,
        .rhs_zero_point = 0,
        .out_zero_point = 0,
    };

    const auto shift = detail::esp_nn::add_left_shift(plan);
    REQUIRE(shift == 2);
    REQUIRE(plan.lhs_multiplier.shift - shift == 0);
    REQUIRE(plan.rhs_multiplier.shift - shift < 0);
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_multiplier.shift = 0;
    plan.rhs_multiplier.shift = 2;
    const auto rhs_shift = detail::esp_nn::add_left_shift(plan);
    REQUIRE(rhs_shift == 2);
    REQUIRE(plan.lhs_multiplier.shift - rhs_shift < 0);
    REQUIRE(plan.rhs_multiplier.shift - rhs_shift == 0);
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));
}
