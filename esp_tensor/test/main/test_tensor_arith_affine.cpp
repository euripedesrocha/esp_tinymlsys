#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <limits>

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

TEST_CASE("prepared affine add matches unprepared add for rescaled domains", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({3, -1, 127, -128});
    auto rhs = std::to_array<std::int8_t>({6, 0, 127, -128});
    auto unprepared = std::to_array<std::int8_t>({0, 0, 0, 0});
    auto prepared = std::to_array<std::int8_t>({0, 0, 0, 0});
    const auto shape = tensor_shape{4};
    const auto lhs_domain = domain::affine<>{.scale = 0.5F, .zero_point = 3};
    const auto rhs_domain = domain::affine<>{.scale = 0.25F, .zero_point = -2};
    const auto out_domain = domain::affine<>{.scale = 0.25F, .zero_point = 5};

    auto lhs_view = tensor_view(lhs, shape, lhs_domain);
    auto rhs_view = tensor_view(rhs, shape, rhs_domain);
    auto unprepared_view = tensor_view(unprepared, shape, out_domain);
    auto prepared_view = tensor_view(prepared, shape, out_domain);
    const auto plan = arith::plan_add(lhs_view.domain(), rhs_view.domain(), prepared_view.domain());

    arith::add(lhs_view, rhs_view, unprepared_view);
    arith::add(lhs_view, rhs_view, prepared_view, plan);

    REQUIRE(prepared == unprepared);
}

TEST_CASE("prepared affine add matches unprepared saturation for default domains", "[affine]") {
    auto lhs = std::to_array<std::int8_t>({120, -120, 10});
    auto rhs = std::to_array<std::int8_t>({20, -20, -3});
    auto unprepared = std::to_array<std::int8_t>({0, 0, 0});
    auto prepared = std::to_array<std::int8_t>({0, 0, 0});
    const auto shape = tensor_shape{3};
    const auto affine_domain = domain::affine<>{};

    auto lhs_view = tensor_view(lhs, shape, affine_domain);
    auto rhs_view = tensor_view(rhs, shape, affine_domain);
    auto unprepared_view = tensor_view(unprepared, shape, affine_domain);
    auto prepared_view = tensor_view(prepared, shape, affine_domain);
    const auto plan = arith::plan_add(lhs_view.domain(), rhs_view.domain(), prepared_view.domain());

    arith::add(lhs_view, rhs_view, unprepared_view);
    arith::add(lhs_view, rhs_view, prepared_view, plan);

    REQUIRE(prepared == unprepared);
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

TEST_CASE("affine add plan prepares esp-nn offsets and shifts", "[affine]") {
    const auto lhs = domain::affine<>{.scale = 0.5F, .zero_point = 3};
    const auto rhs = domain::affine<>{.scale = 0.25F, .zero_point = -2};
    const auto out = domain::affine<>{.scale = 0.25F, .zero_point = 5};
    const auto plan = arith::plan_add(lhs, rhs, out);

    REQUIRE(plan.lhs_zero_point == 3);
    REQUIRE(plan.rhs_zero_point == -2);
    REQUIRE(plan.out_zero_point == 5);
    REQUIRE(plan.lhs_input_offset == -3);
    REQUIRE(plan.rhs_input_offset == 2);
    REQUIRE(plan.lhs_input_shift == 0);
    REQUIRE(plan.rhs_input_shift <= 0);
    REQUIRE(plan.out_multiplier == std::numeric_limits<std::int32_t>::max());
    REQUIRE(plan.out_shift == 0);
    REQUIRE(plan.activation_min == std::numeric_limits<std::int8_t>::lowest());
    REQUIRE(plan.activation_max == std::numeric_limits<std::int8_t>::max());
}

TEST_CASE("esp-nn affine add support accepts only representable int8 input offsets", "[affine]") {
    auto plan = arith::plan_add(domain::affine<>{.zero_point = -127},
                                domain::affine<>{.zero_point = 128}, domain::affine<>{});

    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_input_offset = 128;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_input_offset = 127;
    plan.rhs_input_offset = -129;
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
    auto plan = arith::plan_add(domain::affine<>{}, domain::affine<>{}, domain::affine<>{});
    plan.lhs_shift = 30;
    plan.rhs_shift = 0;
    plan.left_shift = 30;
    plan.lhs_input_shift = 0;
    plan.rhs_input_shift = -30;

    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_shift = 31;
    plan.left_shift = 31;
    plan.lhs_input_shift = 0;
    plan.rhs_input_shift = -31;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_shift = 0;
    plan.rhs_shift = 30;
    plan.left_shift = 30;
    plan.lhs_input_shift = -30;
    plan.rhs_input_shift = 0;
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.rhs_shift = 31;
    plan.left_shift = 31;
    plan.lhs_input_shift = -31;
    plan.rhs_input_shift = 0;
    REQUIRE_FALSE(detail::esp_nn::add_plan_is_supported(plan));
}

TEST_CASE("esp-nn add plan support normalizes positive multiplier shifts", "[affine]") {
    auto plan = arith::plan_add(domain::affine<>{}, domain::affine<>{}, domain::affine<>{});
    plan.lhs_shift = 2;
    plan.rhs_shift = 0;
    plan.left_shift = 2;
    plan.lhs_input_shift = 0;
    plan.rhs_input_shift = -2;

    REQUIRE(plan.left_shift == 2);
    REQUIRE(plan.lhs_input_shift == 0);
    REQUIRE(plan.rhs_input_shift < 0);
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));

    plan.lhs_shift = 0;
    plan.rhs_shift = 2;
    plan.lhs_input_shift = -2;
    plan.rhs_input_shift = 0;
    REQUIRE(plan.left_shift == 2);
    REQUIRE(plan.lhs_input_shift < 0);
    REQUIRE(plan.rhs_input_shift == 0);
    REQUIRE(detail::esp_nn::add_plan_is_supported(plan));
}
