#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

#include "esp_tensor_shape.hpp"

using namespace esp::tensor;

TEST_CASE("tensor_shape default constructs to all zeros", "[tensor_shape]") {
    tensor_shape<3> shape;

    STATIC_REQUIRE(decltype(shape)::rank_value == 3);
    REQUIRE(shape[0] == 0);
    REQUIRE(shape[1] == 0);
    REQUIRE(shape[2] == 0);
    REQUIRE(detail::logical_element_count(shape) == 0);
}

TEST_CASE("tensor_shape constructs from variadic sizes", "[tensor_shape]") {
    const tensor_shape shape{2, 3, 4};

    STATIC_REQUIRE(decltype(shape)::rank_value == 3);
    REQUIRE(shape[0] == 2);
    REQUIRE(shape[1] == 3);
    REQUIRE(shape[2] == 4);
    REQUIRE(detail::logical_element_count(shape) == 24);
}

TEST_CASE("tensor_shape constructs from std array", "[tensor_shape]") {
    const std::array<std::size_t, 4> extents{1, 2, 3, 4};
    const tensor_shape shape{extents};

    STATIC_REQUIRE(decltype(shape)::rank_value == 4);
    REQUIRE(shape[0] == 1);
    REQUIRE(shape[1] == 2);
    REQUIRE(shape[2] == 3);
    REQUIRE(shape[3] == 4);
}

TEST_CASE("tensor_shape supports iteration", "[tensor_shape]") {
    const tensor_shape shape{5, 6, 7};

    const auto expected = std::array<std::size_t, 3>{5, 6, 7};
    REQUIRE(std::equal(shape.begin(), shape.end(), expected.begin(), expected.end()));
}

TEST_CASE("tensor_shape compares equal axis by axis", "[tensor_shape]") {
    const tensor_shape shape_a{2, 3};
    const tensor_shape shape_b{2, 3};
    const tensor_shape shape_c{3, 2};

    REQUIRE(shape_a == shape_b);
    REQUIRE_FALSE(shape_a == shape_c);
}

TEST_CASE("tensor_shape preserves zero extent semantics", "[tensor_shape]") {
    const tensor_shape shape{0, 3};

    REQUIRE(shape[0] == 0);
    REQUIRE(shape[1] == 3);
    REQUIRE(detail::logical_element_count(shape) == 0);
}

TEST_CASE("tensor_shape make_extents produces matching dynamic extents", "[tensor_shape]") {
    constexpr tensor_shape shape{2, 3, 4};
    constexpr auto extents = detail::make_extents(shape);

    STATIC_REQUIRE(extents.rank() == 3);
    STATIC_REQUIRE(extents.extent(0) == 2);
    STATIC_REQUIRE(extents.extent(1) == 3);
    STATIC_REQUIRE(extents.extent(2) == 4);
}
