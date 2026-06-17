#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "esp_tensor_ops_fixed_point.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;

namespace {

auto make_fixed_point_view(auto& values) {
    using value_type = typename std::remove_cvref_t<decltype(values)>::value_type;
    return tensor_view(values, tensor_shape{values.size()}, domain::fixed_point<value_type>{});
}

void require_equal(const auto& actual, const auto& expected) {
    for (std::size_t i = 0; i < actual.size(); ++i) {
        REQUIRE(actual[i] == expected[i]);
    }
}

} // namespace

TEST_CASE("fixed point add identifies esp32s3 accelerated storage widths", "[fixed_point]") {
    STATIC_REQUIRE(detail::esp32s3::accelerated_fixed_point<std::int8_t>);
    STATIC_REQUIRE(detail::esp32s3::accelerated_fixed_point<std::int16_t>);
    STATIC_REQUIRE_FALSE(detail::esp32s3::accelerated_fixed_point<std::int32_t>);
}

TEST_CASE("fixed point add saturates at the domain limits", "[fixed_point]") {
    alignas(16) std::array<std::int16_t, 4> lhs = {
        std::numeric_limits<std::int16_t>::max(),
        std::numeric_limits<std::int16_t>::min(),
        16384,
        -16384,
    };
    alignas(16) std::array<std::int16_t, 4> rhs = {1, -1, 16384, -16384};
    alignas(16) std::array<std::int16_t, 4> out = {0, 0, 0, 0};

    arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs), make_fixed_point_view(out));

    require_equal(out, std::array<std::int16_t, 4>{
                           std::numeric_limits<std::int16_t>::max(),
                           std::numeric_limits<std::int16_t>::min(),
                           std::numeric_limits<std::int16_t>::max(),
                           std::numeric_limits<std::int16_t>::min(),
                       });
}

TEST_CASE("fixed point add handles vector-width tails correctly", "[fixed_point]") {
    SECTION("s16 bulk and tail") {
        alignas(16) std::array<std::int16_t, 10> lhs = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        alignas(16) std::array<std::int16_t, 10> rhs = {10, 9, 8, 7, 6, 5, 4, 3, 2, 1};
        alignas(16) std::array<std::int16_t, 10> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int16_t, 10>{11, 11, 11, 11, 11, 11, 11, 11, 11, 11});
    }

    SECTION("s8 bulk and tail") {
        alignas(16) std::array<std::int8_t, 17> lhs = {1, 1, 1, 1, 1, 1, 1, 1, 1,
                                                       1, 1, 1, 1, 1, 1, 1, 1};
        alignas(16) std::array<std::int8_t, 17> rhs = {2, 2, 2, 2, 2, 2, 2, 2, 2,
                                                       2, 2, 2, 2, 2, 2, 2, 2};
        alignas(16) std::array<std::int8_t, 17> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(
            out, std::array<std::int8_t, 17>{3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3});
    }
}

TEST_CASE("fixed point add supports signed storage widths", "[fixed_point]") {
    SECTION("s16") {
        alignas(16) std::array<std::int16_t, 2> lhs = {16384, -16384};
        alignas(16) std::array<std::int16_t, 2> rhs = {8192, -8192};
        alignas(16) std::array<std::int16_t, 2> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int16_t, 2>{24576, -24576});
    }

    SECTION("s8") {
        alignas(16) std::array<std::int8_t, 2> lhs = {32, -32};
        alignas(16) std::array<std::int8_t, 2> rhs = {16, -16};
        alignas(16) std::array<std::int8_t, 2> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int8_t, 2>{48, -48});
    }

    SECTION("s32 ordinary values") {
        alignas(16) std::array<std::int32_t, 4> lhs = {1000, -2000, 3000, -4000};
        alignas(16) std::array<std::int32_t, 4> rhs = {500, -500, -1000, 1000};
        alignas(16) std::array<std::int32_t, 4> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int32_t, 4>{1500, -2500, 2000, -3000});
    }

    SECTION("s32 saturation") {
        alignas(16) std::array<std::int32_t, 5> lhs = {
            1, 2, 3, 4, std::numeric_limits<std::int32_t>::max(),
        };
        alignas(16) std::array<std::int32_t, 5> rhs = {5, 4, 3, 2, 1};
        alignas(16) std::array<std::int32_t, 5> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int32_t, 5>{
                               6,
                               6,
                               6,
                               6,
                               std::numeric_limits<std::int32_t>::max(),
                           });
    }
}

TEST_CASE("fixed point add handles empty tensors", "[fixed_point]") {
    alignas(16) std::array<std::int16_t, 1> lhs = {42};
    alignas(16) std::array<std::int16_t, 1> rhs = {1};
    alignas(16) std::array<std::int16_t, 1> out = {99};

    arith::add(tensor_view(std::span<std::int16_t>(lhs.data(), 0), tensor_shape{0},
                           domain::fixed_point<std::int16_t>{}),
               tensor_view(std::span<std::int16_t>(rhs.data(), 0), tensor_shape{0},
                           domain::fixed_point<std::int16_t>{}),
               tensor_view(std::span<std::int16_t>(out.data(), 0), tensor_shape{0},
                           domain::fixed_point<std::int16_t>{}));

    REQUIRE(out[0] == 99);
}

TEST_CASE("fixed point add uses aligned accelerated path on esp32s3", "[fixed_point]") {
    if constexpr (hal::active_target() == hal::target::esp32s3) {
        alignas(16) std::array<std::int16_t, 8> lhs = {1, 2, 3, 4, 5, 6, 7, 8};
        alignas(16) std::array<std::int16_t, 8> rhs = {8, 7, 6, 5, 4, 3, 2, 1};
        alignas(16) std::array<std::int16_t, 8> out = {};

        arith::add(make_fixed_point_view(lhs), make_fixed_point_view(rhs),
                   make_fixed_point_view(out));

        require_equal(out, std::array<std::int16_t, 8>{9, 9, 9, 9, 9, 9, 9, 9});
    } else {
        SUCCEED("aligned accelerated path is only exercised on esp32s3");
    }
}

TEST_CASE("fixed point add validates shape compatibility at the ops facade", "[fixed_point]") {
    alignas(16) std::array<std::int16_t, 2> lhs = {1, 2};
    alignas(16) std::array<std::int16_t, 3> rhs = {3, 4, 5};
    alignas(16) std::array<std::int16_t, 2> out = {0, 0};

    REQUIRE_THROWS_MATCHES(
        arith::add(tensor_view(lhs, tensor_shape{2}, domain::fixed_point<std::int16_t>{}),
                   tensor_view(rhs, tensor_shape{3}, domain::fixed_point<std::int16_t>{}),
                   tensor_view(out, tensor_shape{2}, domain::fixed_point<std::int16_t>{})),
        std::invalid_argument,
        Catch::Matchers::Message("tensor add requires matching input shapes"));
}

TEST_CASE("fixed point add requires aligned buffers on accelerated targets", "[fixed_point]") {
    if constexpr (hal::active_target() == hal::target::esp32s3) {
        constexpr std::size_t element_count = 8;
        std::array<std::byte, (sizeof(std::int16_t) * element_count) + 1> lhs_storage = {};
        std::array<std::byte, (sizeof(std::int16_t) * element_count) + 1> rhs_storage = {};
        std::array<std::byte, (sizeof(std::int16_t) * element_count) + 1> out_storage = {};

        auto* lhs = reinterpret_cast<std::int16_t*>(lhs_storage.data() + 1);
        auto* rhs = reinterpret_cast<std::int16_t*>(rhs_storage.data() + 1);
        auto* out = reinterpret_cast<std::int16_t*>(out_storage.data() + 1);

        REQUIRE_THROWS_MATCHES(
            arith::add(
                tensor_view(std::span<std::int16_t>(lhs, element_count),
                            tensor_shape{element_count}, domain::fixed_point<std::int16_t>{}),
                tensor_view(std::span<std::int16_t>(rhs, element_count),
                            tensor_shape{element_count}, domain::fixed_point<std::int16_t>{}),
                tensor_view(std::span<std::int16_t>(out, element_count),
                            tensor_shape{element_count}, domain::fixed_point<std::int16_t>{})),
            std::invalid_argument,
            Catch::Matchers::Message("esp32s3 fixed-point add requires aligned lhs storage"));
    } else {
        SUCCEED("aligned-buffer validation is only required on accelerated targets");
    }
}
