#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <array>
#include <cstdint>
#include <utility>

#include "esp_tensor.hpp"
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

template <class Lhs, class Rhs, class Out>
concept add_invocable = requires(Lhs&& lhs, Rhs&& rhs, Out&& out) {
    add(std::forward<Lhs>(lhs), std::forward<Rhs>(rhs), std::forward<Out>(out));
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

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 32);
    REQUIRE(out[1] == -32);
    REQUIRE(out[2] == -3);
}

TEST_CASE("no quantization add supports floating point tensors", "[ops]") {
    auto lhs = std::to_array<float>({0.5F, -1.25F});
    auto rhs = std::to_array<float>({0.25F, 0.5F});
    auto out = std::to_array<float>({0.0F, 0.0F});
    const auto shape = tensor_shape{2};

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 0.75F);
    REQUIRE(out[1] == -0.75F);
}

TEST_CASE("add accepts view operands and rejects owning tensors", "[ops]") {
    auto out = std::to_array<std::int8_t>({0});
    const auto shape = tensor_shape{1};
    using view_type = decltype(tensor_view(out, shape));
    using const_view_type = decltype(tensor_view(std::as_const(out), shape));
    using owner_type = tensor<domain::no_quantization<std::int8_t>, tensor_shape<1>>;

    STATIC_REQUIRE(tensor_view_like<view_type>);
    STATIC_REQUIRE(tensor_view_like<const_view_type>);
    STATIC_REQUIRE(std::convertible_to<decltype(std::declval<view_type&>().data()), std::int8_t*>);
    STATIC_REQUIRE(
        std::convertible_to<decltype(std::declval<const_view_type&>().data()), const std::int8_t*>);
    STATIC_REQUIRE_FALSE(tensor_view_like<owner_type>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type&, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type, view_type&, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type, view_type, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type&, view_type>);
    STATIC_REQUIRE_FALSE(add_invocable<owner_type&, owner_type&, owner_type&>);
}

TEST_CASE("add accepts const-backed input views", "[ops]") {
    constexpr auto lhs = std::to_array<std::int8_t>({12, -12, 5});
    constexpr auto rhs = std::to_array<std::int8_t>({20, -20, -8});
    auto out = std::to_array<std::int8_t>({0, 0, 0});
    const auto shape = tensor_shape{3};

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 32);
    REQUIRE(out[1] == -32);
    REQUIRE(out[2] == -3);
}

TEST_CASE("add combines constexpr array views with owning tensor views", "[ops]") {
    constexpr auto lhs = std::to_array<std::int8_t>({12, -12, 5});
    tensor rhs(tensor_shape{3}, domain::no_quantization<std::int8_t>{});
    tensor out(tensor_shape{3}, domain::no_quantization<std::int8_t>{});
    rhs.data()[0] = 20;
    rhs.data()[1] = -20;
    rhs.data()[2] = -8;

    auto lhs_view = tensor_view(lhs, tensor_shape{3});
    auto rhs_view = tensor_view(rhs);
    auto out_view = tensor_view(out);
    add(lhs_view, rhs_view, out_view);

    REQUIRE(out.data()[0] == 32);
    REQUIRE(out.data()[1] == -32);
    REQUIRE(out.data()[2] == -3);
}

TEST_CASE("no quantization add validates matching runtime input shapes", "[ops]") {
    auto lhs = std::to_array<std::int16_t>({1, 2});
    auto rhs = std::to_array<std::int16_t>({3, 4, 5});
    auto out = std::to_array<std::int16_t>({0, 0});

    auto lhs_view = tensor_view(lhs, tensor_shape{2});
    auto rhs_view = tensor_view(rhs, tensor_shape{3});
    auto out_view = tensor_view(out, tensor_shape{2});
    REQUIRE_THROWS_MATCHES(add(lhs_view, rhs_view, out_view), std::invalid_argument,
                           Catch::Matchers::Message("tensor add requires matching input shapes"));
}

TEST_CASE("no quantization add validates the output runtime shape", "[ops]") {
    auto lhs = std::to_array<std::int16_t>({1, 2});
    auto rhs = std::to_array<std::int16_t>({3, 4});
    auto out = std::to_array<std::int16_t>({0, 0, 0});

    auto lhs_view = tensor_view(lhs, tensor_shape{2});
    auto rhs_view = tensor_view(rhs, tensor_shape{2});
    auto out_view = tensor_view(out, tensor_shape{3});
    REQUIRE_THROWS_MATCHES(
        add(lhs_view, rhs_view, out_view), std::invalid_argument,
        Catch::Matchers::Message("tensor add requires output shape to match inputs"));
}
