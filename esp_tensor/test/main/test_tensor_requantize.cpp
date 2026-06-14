#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <array>
#include <cstdint>
#include <utility>

#include "esp_tensor_domain.hpp"
#include "esp_tensor_ops.hpp"
#include "esp_tensor_shape.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;
using Catch::Matchers::Message;
using Catch::Matchers::WithinAbs;

namespace {

template <class Input, class Output>
concept requantize_invocable = requires(Input&& input, Output&& output) {
    requantize(std::forward<Input>(input), std::forward<Output>(output));
};

} // namespace

TEST_CASE("requantize is defined for fixed point to float domain conversions", "[requantize]") {
    auto q15 = std::to_array<std::int16_t>({0});
    auto q7 = std::to_array<std::int8_t>({0});
    auto f32 = std::to_array<float>({0.0F});
    auto f64 = std::to_array<double>({0.0});
    const auto shape = tensor_shape{1};

    using q15_view = decltype(tensor_view(q15, shape, domain::fixed_point<std::int16_t>{}));
    using q7_view = decltype(tensor_view(q7, shape, domain::fixed_point<std::int8_t>{}));
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

    requantize(tensor_view(input, shape, domain::fixed_point<std::int16_t>{}),
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

    requantize(tensor_view(input, shape, domain::fixed_point<std::int8_t>{}),
               tensor_view(output, shape, domain::no_quantization<float>{}));

    REQUIRE(output[0] == 0.0F);
    REQUIRE(output[1] == 0.5F);
    REQUIRE(output[2] == -0.5F);
    REQUIRE_THAT(output[3], WithinAbs(127.0F / 128.0F, 0.000001F));
    REQUIRE(output[4] == -1.0F);
}

TEST_CASE("requantize validates matching logical shapes", "[requantize]") {
    auto input = std::to_array<std::int16_t>({0, 16384});
    auto output = std::to_array<float>({0.0F, 0.0F});

    auto input_view = tensor_view(input, tensor_shape{1}, domain::fixed_point<std::int16_t>{});
    auto output_view = tensor_view(output, tensor_shape{2}, domain::no_quantization<float>{});

    REQUIRE_THROWS_MATCHES(requantize(input_view, output_view), std::invalid_argument,
                           Message("tensor requantize requires matching input and output shapes"));
}
