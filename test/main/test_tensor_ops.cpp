#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <array>
#include <cstdint>

#include "esp_tensor_ops.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;

namespace {

struct custom_domain {
    using value_type = float;
};

template <class T> struct other_domain {
    using value_type = T;
};

} // namespace

TEST_CASE("no quantization add concepts dispatch only to no quantization domains", "[ops]") {
    STATIC_REQUIRE(detail::no_quantization_domain<domain::no_quantization<float>>);
    STATIC_REQUIRE(detail::no_quantization_domain<domain::no_quantization<std::int16_t>>);
    STATIC_REQUIRE_FALSE(detail::no_quantization_domain<custom_domain>);
    STATIC_REQUIRE_FALSE(detail::no_quantization_domain<other_domain<std::int16_t>>);
}

TEST_CASE("no quantization add uses the reference arithmetic path", "[ops]") {
    auto lhs = std::to_array<std::int8_t>({12, -12, 5});
    auto rhs = std::to_array<std::int8_t>({20, -20, -8});
    auto out = std::to_array<std::int8_t>({0, 0, 0});
    const auto shape = tensor_shape{3};

    add(tensor_view(lhs, shape), tensor_view(rhs, shape), tensor_view(out, shape));

    REQUIRE(out[0] == 32);
    REQUIRE(out[1] == -32);
    REQUIRE(out[2] == -3);
}

TEST_CASE("no quantization add supports floating point tensors", "[ops]") {
    auto lhs = std::to_array<float>({0.5F, -1.25F});
    auto rhs = std::to_array<float>({0.25F, 0.5F});
    auto out = std::to_array<float>({0.0F, 0.0F});
    const auto shape = tensor_shape{2};

    add(tensor_view(lhs, shape), tensor_view(rhs, shape), tensor_view(out, shape));

    REQUIRE(out[0] == 0.75F);
    REQUIRE(out[1] == -0.75F);
}

TEST_CASE("no quantization add validates matching runtime input shapes", "[ops]") {
    auto lhs = std::to_array<std::int16_t>({1, 2});
    auto rhs = std::to_array<std::int16_t>({3, 4, 5});
    auto out = std::to_array<std::int16_t>({0, 0});

    REQUIRE_THROWS_MATCHES(add(tensor_view(lhs, tensor_shape{2}), tensor_view(rhs, tensor_shape{3}),
                               tensor_view(out, tensor_shape{2})),
                           std::invalid_argument,
                           Catch::Matchers::Message("tensor add requires matching input shapes"));
}

TEST_CASE("no quantization add validates the output runtime shape", "[ops]") {
    auto lhs = std::to_array<std::int16_t>({1, 2});
    auto rhs = std::to_array<std::int16_t>({3, 4});
    auto out = std::to_array<std::int16_t>({0, 0, 0});

    REQUIRE_THROWS_MATCHES(
        add(tensor_view(lhs, tensor_shape{2}), tensor_view(rhs, tensor_shape{2}),
            tensor_view(out, tensor_shape{3})),
        std::invalid_argument,
        Catch::Matchers::Message("tensor add requires output shape to match inputs"));
}
