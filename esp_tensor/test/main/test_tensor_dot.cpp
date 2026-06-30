#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>

#include "esp_tensor_ops_fixed_point.hpp"
#include "esp_tensor_view.hpp"

using Catch::Matchers::WithinAbs;

using namespace esp::tensor;

namespace {

template <class Lhs, class Rhs, class Out>
concept dot_invocable = requires(Lhs&& lhs, Rhs&& rhs, Out&& out) {
    linear_algebra::dot(std::forward<Lhs>(lhs), std::forward<Rhs>(rhs), std::forward<Out>(out));
};

} // namespace

TEST_CASE("dot concepts accept supported domains and reject mixed output domains", "[dot]") {
    auto f32 = std::to_array<float>({0.0F});
    auto f64 = std::to_array<double>({0.0});
    auto s8 = std::to_array<std::int8_t>({0});
    auto s32 = std::to_array<std::int32_t>({0});
    const auto shape = tensor_shape{1};

    using f32_view = decltype(tensor_view(f32, shape, domain::no_quantization<float>{}));
    using f64_view = decltype(tensor_view(f64, shape, domain::no_quantization<double>{}));
    using s8_view = decltype(tensor_view(s8, shape, domain::fixed_point<std::int8_t>{}));
    using s32_view = decltype(tensor_view(s32, shape, domain::fixed_point<std::int32_t>{}));

    STATIC_REQUIRE(dot_invocable<f32_view, f32_view, f32_view>);
    STATIC_REQUIRE_FALSE(dot_invocable<f32_view, f32_view, f64_view>);
    STATIC_REQUIRE(dot_invocable<s8_view, s8_view, s8_view>);
    STATIC_REQUIRE_FALSE(dot_invocable<s8_view, s8_view, s32_view>);
    STATIC_REQUIRE_FALSE(dot_invocable<s8_view, s32_view, s32_view>);
}

TEST_CASE("unquantized float dot multiplies and accumulates", "[dot]") {
    auto lhs = std::to_array<float>({1.5F, -2.0F, 3.0F});
    auto rhs = std::to_array<float>({2.0F, 4.0F, -1.0F});
    auto out = std::to_array<float>({0.0F});
    const auto shape = tensor_shape{3};

    const auto lhs_view = tensor_view(lhs, shape, domain::no_quantization<float>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::no_quantization<float>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::no_quantization<float>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE_THAT(out[0], WithinAbs(-8.0F, 0.0001F));
}

TEST_CASE("unquantized double dot uses the scalar path", "[dot]") {
    auto lhs = std::to_array<double>({1.5, -2.0, 3.0});
    auto rhs = std::to_array<double>({2.0, 4.0, -1.0});
    auto out = std::to_array<double>({0.0});
    const auto shape = tensor_shape{3};

    const auto lhs_view = tensor_view(lhs, shape, domain::no_quantization<double>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::no_quantization<double>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::no_quantization<double>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE_THAT(out[0], WithinAbs(-8.0, 0.0001));
}

TEST_CASE("fixed int8 dot rescales Q7 products into Q7 output", "[dot]") {
    alignas(16) auto lhs = std::to_array<std::int8_t>({100, 100});
    alignas(16) auto rhs = std::to_array<std::int8_t>({2, 2});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{2};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int8_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int8_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int8_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 3);
}

TEST_CASE("fixed int8 dot handles Q7 vector prefix with scalar tail", "[dot]") {
    alignas(16) auto lhs =
        std::to_array<std::int8_t>({8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8});
    alignas(16) auto rhs =
        std::to_array<std::int8_t>({8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8, 8});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{17};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int8_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int8_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int8_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 9);
}

TEST_CASE("fixed int16 dot rescales Q15 products into Q15 output", "[dot]") {
    auto lhs = std::to_array<std::int16_t>({16384, 8192, 0, 0});
    auto rhs = std::to_array<std::int16_t>({16384, 8192, 0, 0});
    auto out = std::to_array<std::int16_t>({0});
    const auto shape = tensor_shape{4};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int16_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int16_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int16_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 10240);
}

TEST_CASE("fixed int16 dot handles Q15 non-vector-sized inputs", "[dot]") {
    auto lhs = std::to_array<std::int16_t>({16384, 8192, 0, 0, 8192});
    auto rhs = std::to_array<std::int16_t>({16384, 8192, 0, 0, 8192});
    auto out = std::to_array<std::int16_t>({0});
    const auto shape = tensor_shape{5};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int16_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int16_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int16_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 12288);
}

TEST_CASE("fixed int16 dot handles Q15 inputs longer than one SIMD batch", "[dot]") {
    alignas(16) auto lhs =
        std::to_array<std::int16_t>({8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192});
    alignas(16) auto rhs =
        std::to_array<std::int16_t>({8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192});
    auto out = std::to_array<std::int16_t>({0});
    const auto shape = tensor_shape{9};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int16_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int16_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int16_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 18432);
}

TEST_CASE("fixed int16 dot saturates after rescaling into Q15 output", "[dot]") {
    auto lhs = std::to_array<std::int16_t>({std::numeric_limits<std::int16_t>::max(),
                                            std::numeric_limits<std::int16_t>::max(),
                                            std::numeric_limits<std::int16_t>::max()});
    auto rhs = std::to_array<std::int16_t>({std::numeric_limits<std::int16_t>::max(),
                                            std::numeric_limits<std::int16_t>::max(),
                                            std::numeric_limits<std::int16_t>::max()});
    auto out = std::to_array<std::int16_t>({0});
    const auto shape = tensor_shape{3};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int16_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int16_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int16_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == std::numeric_limits<std::int16_t>::max());
}

TEST_CASE("fixed int8 dot saturates after rescaling into Q7 output", "[dot]") {
    alignas(16) auto lhs = std::to_array<std::int8_t>({127, 127, 127, 127});
    alignas(16) auto rhs = std::to_array<std::int8_t>({127, 127, 127, 127});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{4};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int8_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int8_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int8_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == std::numeric_limits<std::int8_t>::max());
}

TEST_CASE("fixed int8 dot handles vectors shorter than batch size", "[dot]") {
    alignas(16) auto lhs = std::to_array<std::int8_t>({8, 8});
    alignas(16) auto rhs = std::to_array<std::int8_t>({8, 8});
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{2};

    const auto lhs_view = tensor_view(lhs, shape, domain::fixed_point<std::int8_t>{});
    const auto rhs_view = tensor_view(rhs, shape, domain::fixed_point<std::int8_t>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::fixed_point<std::int8_t>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 1);
}

TEST_CASE("dot produces zero for empty inputs", "[dot]") {
    auto lhs = std::to_array<float>({1.0F});
    auto rhs = std::to_array<float>({1.0F});
    auto out = std::to_array<float>({99.0F});

    const auto lhs_view = tensor_view(std::span<float>(lhs.data(), 0), tensor_shape{0},
                                      domain::no_quantization<float>{});
    const auto rhs_view = tensor_view(std::span<float>(rhs.data(), 0), tensor_shape{0},
                                      domain::no_quantization<float>{});
    auto out_view = tensor_view(out, tensor_shape{1}, domain::no_quantization<float>{});

    linear_algebra::dot(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 0.0F);
}
