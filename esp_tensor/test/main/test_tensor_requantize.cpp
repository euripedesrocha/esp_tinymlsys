#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <utility>

#include "esp_tensor_domain.hpp"
#include "esp_tensor_ops_fixed_point.hpp"
#include "esp_tensor_shape.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;
using Catch::Matchers::WithinAbs;

namespace {

template <class Input, class Output>
concept requantize_invocable = requires(Input&& input, Output&& output) {
    quantization::requantize(std::forward<Input>(input), std::forward<Output>(output));
};

} // namespace

TEST_CASE("requantize_fixed_point converts scalar fixed point values", "[requantize]") {
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 31>(std::int64_t{1} << 30) ==
                   64);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int16_t, 30>(std::int64_t{1} << 29) ==
                   16384);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int16_t, 63>(std::int64_t{1} << 62) ==
                   16384);
}

TEST_CASE("requantize_fixed_point converts product accumulators", "[requantize]") {
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(std::int64_t{1} << 13) ==
                   64);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int16_t, 30>(std::int64_t{1} << 29) ==
                   16384);
}

TEST_CASE("requantize_fixed_point rounds when reducing fractional width", "[requantize]") {
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(63) == 0);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(64) == 1);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(191) == 1);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(192) == 2);
}

TEST_CASE("requantize_fixed_point shifts left when increasing fractional width", "[requantize]") {
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int16_t, 7>(64) == 16384);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int16_t, 7>(127) == 32512);
}

TEST_CASE("requantize_fixed_point saturates scalar fixed point values", "[requantize]") {
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 31>(
                       std::numeric_limits<std::int64_t>::max()) == 127);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 31>(
                       std::numeric_limits<std::int64_t>::min()) == -128);
    STATIC_REQUIRE(quantization::requantize_fixed_point<std::int8_t, 14>(std::int64_t{1} << 14) ==
                   127);
}

TEST_CASE("requantize is defined for fixed point to float domain conversions", "[requantize]") {
    auto q15 = std::to_array<std::int16_t>({0});
    auto q7_values = std::to_array<std::int8_t>({0});
    auto f32 = std::to_array<float>({0.0F});
    auto f64 = std::to_array<double>({0.0});
    const auto shape = tensor_shape{1};

    using q15_view = decltype(tensor_view(q15, shape, domain::fixed_point<std::int16_t>{}));
    using q7_view = decltype(tensor_view(q7_values, shape, domain::fixed_point<std::int8_t>{}));
    using f32_view = decltype(tensor_view(f32, shape, domain::no_quantization<float>{}));
    using f64_view = decltype(tensor_view(f64, shape, domain::no_quantization<double>{}));

    STATIC_REQUIRE(requantize_invocable<q15_view, f32_view>);
    STATIC_REQUIRE(requantize_invocable<q7_view, f32_view>);
    STATIC_REQUIRE(requantize_invocable<q15_view, f64_view>);
}

TEST_CASE("requantize converts Q15 fixed point values to float", "[requantize]") {
    auto input = std::to_array<std::int16_t>({0, 16384, -16384, 32767, -32768});
    auto output = std::to_array<float>({0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
    const auto shape = tensor_shape{input.size()};

    quantization::requantize(tensor_view(input, shape, domain::fixed_point<std::int16_t>{}),
                             tensor_view(output, shape, domain::no_quantization<float>{}));

    REQUIRE(output[0] == 0.0F);
    REQUIRE(output[1] == 0.5F);
    REQUIRE(output[2] == -0.5F);
    REQUIRE_THAT(output[3], WithinAbs(32767.0F / 32768.0F, 0.000001F));
    REQUIRE(output[4] == -1.0F);
}

TEST_CASE("requantize uses the source fixed point fractional width", "[requantize]") {
    auto input = std::to_array<std::int8_t>({0, 64, -64, 127, -128});
    auto output = std::to_array<float>({0.0F, 0.0F, 0.0F, 0.0F, 0.0F});
    const auto shape = tensor_shape{input.size()};

    quantization::requantize(tensor_view(input, shape, domain::fixed_point<std::int8_t>{}),
                             tensor_view(output, shape, domain::no_quantization<float>{}));

    REQUIRE(output[0] == 0.0F);
    REQUIRE(output[1] == 0.5F);
    REQUIRE(output[2] == -0.5F);
    REQUIRE_THAT(output[3], WithinAbs(127.0F / 128.0F, 0.000001F));
    REQUIRE(output[4] == -1.0F);
}

TEST_CASE("requantize converts Q15 fixed point values to double", "[requantize]") {
    auto input = std::to_array<std::int16_t>({0, 16384, -16384, 32767, -32768});
    auto output = std::to_array<double>({0.0, 0.0, 0.0, 0.0, 0.0});
    const auto shape = tensor_shape{input.size()};

    quantization::requantize(tensor_view(input, shape, domain::fixed_point<std::int16_t>{}),
                             tensor_view(output, shape, domain::no_quantization<double>{}));

    REQUIRE(output[0] == 0.0);
    REQUIRE(output[1] == 0.5);
    REQUIRE(output[2] == -0.5);
    REQUIRE_THAT(output[3], WithinAbs(32767.0 / 32768.0, 0.000001));
    REQUIRE(output[4] == -1.0);
}

TEST_CASE("requantize handles extreme fixed point values", "[requantize]") {
    auto input = std::to_array<std::int8_t>({-128, 127});
    auto output = std::to_array<float>({0.0F, 0.0F});
    const auto shape = tensor_shape{input.size()};

    quantization::requantize(tensor_view(input, shape, domain::fixed_point<std::int8_t>{}),
                             tensor_view(output, shape, domain::no_quantization<float>{}));

    REQUIRE(output[0] == -1.0F);
    REQUIRE_THAT(output[1], WithinAbs(127.0F / 128.0F, 0.000001F));
}
