#include <catch2/catch_test_macros.hpp>

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
    arith::add(std::forward<Lhs>(lhs), std::forward<Rhs>(rhs), std::forward<Out>(out));
};

} // namespace

TEST_CASE("no quantization add concepts dispatch only to no quantization domains", "[ops]") {
    STATIC_REQUIRE(detail::no_quantization_domain<domain::no_quantization<float>>);
    STATIC_REQUIRE_FALSE(detail::no_quantization_domain<custom_domain>);
    STATIC_REQUIRE_FALSE(detail::no_quantization_domain<other_domain<std::int16_t>>);
}

TEST_CASE("no quantization add uses the reference arithmetic path", "[ops]") {
    auto lhs = std::to_array<float>({12.0F, -12.0F, 5.0F});
    auto rhs = std::to_array<float>({20.0F, -20.0F, -8.0F});
    auto out = std::to_array<float>({0.0F, 0.0F, 0.0F});
    const auto shape = tensor_shape{3};

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    arith::add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 32.0F);
    REQUIRE(out[1] == -32.0F);
    REQUIRE(out[2] == -3.0F);
}

TEST_CASE("no quantization add supports floating point tensors", "[ops]") {
    auto lhs = std::to_array<float>({0.5F, -1.25F});
    auto rhs = std::to_array<float>({0.25F, 0.5F});
    auto out = std::to_array<float>({0.0F, 0.0F});
    const auto shape = tensor_shape{2};

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    arith::add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 0.75F);
    REQUIRE(out[1] == -0.75F);
}

TEST_CASE("add accepts view operands and rejects owning tensors", "[ops]") {
    auto out = std::to_array<float>({0.0F});
    const auto shape = tensor_shape{1};
    using view_type = decltype(tensor_view(out, shape));
    using const_view_type = decltype(tensor_view(std::as_const(out), shape));
    using owner_type = tensor<domain::no_quantization<float>, tensor_shape<1>>;

    STATIC_REQUIRE(tensor_view_like<view_type>);
    STATIC_REQUIRE(tensor_view_like<const_view_type>);
    STATIC_REQUIRE(std::convertible_to<decltype(std::declval<view_type&>().data()), float*>);
    STATIC_REQUIRE(
        std::convertible_to<decltype(std::declval<const_view_type&>().data()), const float*>);
    STATIC_REQUIRE_FALSE(tensor_view_like<owner_type>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type&, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type, view_type&, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type, view_type, view_type&>);
    STATIC_REQUIRE(add_invocable<view_type&, view_type&, view_type>);
    STATIC_REQUIRE_FALSE(add_invocable<owner_type&, owner_type&, owner_type&>);
}

TEST_CASE("add rejects mixed value types", "[ops]") {
    auto f32 = std::to_array<float>({1.0F});
    auto f64 = std::to_array<double>({1.0});
    auto out = std::to_array<float>({0.0F});
    const auto shape = tensor_shape{1};

    using f32_view = decltype(tensor_view(f32, shape, domain::no_quantization<float>{}));
    using f64_view = decltype(tensor_view(f64, shape, domain::no_quantization<double>{}));
    using out_view = decltype(tensor_view(out, shape, domain::no_quantization<float>{}));

    STATIC_REQUIRE_FALSE(add_invocable<f32_view, f64_view, out_view>);
}

TEST_CASE("add accepts const-backed input views", "[ops]") {
    constexpr auto lhs = std::to_array<float>({12.0F, -12.0F, 5.0F});
    constexpr auto rhs = std::to_array<float>({20.0F, -20.0F, -8.0F});
    auto out = std::to_array<float>({0.0F, 0.0F, 0.0F});
    const auto shape = tensor_shape{3};

    auto lhs_view = tensor_view(lhs, shape);
    auto rhs_view = tensor_view(rhs, shape);
    auto out_view = tensor_view(out, shape);
    arith::add(lhs_view, rhs_view, out_view);

    REQUIRE(out[0] == 32.0F);
    REQUIRE(out[1] == -32.0F);
    REQUIRE(out[2] == -3.0F);
}

TEST_CASE("add combines constexpr array views with owning tensor views", "[ops]") {
    constexpr auto lhs = std::to_array<float>({12.0F, -12.0F, 5.0F});
    tensor rhs(tensor_shape{3}, domain::no_quantization<float>{});
    tensor out(tensor_shape{3}, domain::no_quantization<float>{});
    rhs.data()[0] = 20.0F;
    rhs.data()[1] = -20.0F;
    rhs.data()[2] = -8.0F;

    auto lhs_view = tensor_view(lhs, tensor_shape{3});
    auto rhs_view = tensor_view(rhs);
    auto out_view = tensor_view(out);
    arith::add(lhs_view, rhs_view, out_view);

    REQUIRE(out.data()[0] == 32.0F);
    REQUIRE(out.data()[1] == -32.0F);
    REQUIRE(out.data()[2] == -3.0F);
}
