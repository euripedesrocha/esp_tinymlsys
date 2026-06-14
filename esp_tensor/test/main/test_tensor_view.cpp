#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_exception.hpp>

#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "esp_tensor.hpp"
#include "esp_tensor_domain.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;

namespace {

struct calibration_domain {
    using value_type = std::int16_t;
    int profile = 0;
};

struct broken_tensor_like {
    using domain_type = domain::no_quantization<std::int16_t>;
    using shape_type = tensor_shape<2>;
    domain_type::value_type* storage = nullptr;
    std::size_t logical_size = 0;
    shape_type logical_shape;
    domain_type logical_domain{};

    domain_type::value_type* data() const { return storage; }
    const shape_type& shape() const { return logical_shape; }
    const domain_type& domain() const { return logical_domain; }
    std::size_t size() const { return logical_size; }
};

struct calibrated_tensor_like {
    using domain_type = calibration_domain;
    using shape_type = tensor_shape<2>;
    domain_type::value_type* storage = nullptr;
    std::size_t logical_size = 0;
    shape_type logical_shape;
    domain_type logical_domain{};

    domain_type::value_type* data() const { return storage; }
    const shape_type& shape() const { return logical_shape; }
    const domain_type& domain() const { return logical_domain; }
    std::size_t size() const { return logical_size; }
};

constexpr auto constexpr_view_values = std::to_array<float>({1.0F, 2.0F, 3.0F, 4.0F});
constexpr auto constexpr_view_shape = tensor_shape{2, 2};
constexpr auto constexpr_view_domain = domain::no_quantization<float>{};
constexpr auto constexpr_view =
    tensor_view(constexpr_view_values, constexpr_view_shape, constexpr_view_domain);

} // namespace

TEST_CASE("tensor view carries runtime shape and domain", "[tensor_view]") {
    std::int16_t values[] = {1, 2, 3, 4, 5, 6};
    calibration_domain calibration{.profile = 7};
    tensor_view<std::int16_t, calibration_domain, tensor_shape<2>> view(values, tensor_shape{2, 3},
                                                                        calibration);

    STATIC_REQUIRE(decltype(view)::rank() == 2);
    REQUIRE(view.data() == values);
    REQUIRE(view.shape() == tensor_shape{2, 3});
    REQUIRE(view.size() == 6);
    REQUIRE(view.domain().profile == 7);
}

TEST_CASE("tensor view carries unconstrained domain metadata", "[tensor_view]") {
    std::int16_t values[] = {1, 2, 3};
    tensor_view view(values, tensor_shape{3}, calibration_domain{.profile = 7});

    REQUIRE(view.size() == 3);
    REQUIRE(view.domain().profile == 7);
}

TEST_CASE("tensor view accepts one-dimensional contiguous spans", "[tensor_view]") {
    auto values = std::to_array<std::int16_t>({1, 2, 3, 4});

    tensor_view vector_view(std::span{values});
    REQUIRE(vector_view.data() == values.data());
    REQUIRE(vector_view.shape() == tensor_shape{4});
    REQUIRE(vector_view.size() == 4);
    REQUIRE_FALSE(vector_view.empty());
}

TEST_CASE("tensor view supports flat iteration over contiguous storage", "[tensor_view]") {
    auto values = std::to_array<std::int16_t>({1, 2, 3, 4});
    tensor_view view(values, tensor_shape{2, 2}, domain::no_quantization<std::int16_t>{});
    using const_iterator = decltype(std::as_const(view).begin());
    using const_reference = decltype(std::as_const(view)[0, 0]);

    STATIC_REQUIRE(std::same_as<const_iterator, const std::int16_t*>);
    STATIC_REQUIRE(std::same_as<const_reference, const std::int16_t&>);

    std::int16_t next = 10;
    for (auto& value : view) {
        value = next++;
    }

    const auto& const_view = view;
    std::int32_t sum = 0;
    for (const auto value : const_view) {
        sum += value;
    }

    REQUIRE(view.begin() == values.data());
    REQUIRE(view.end() == values.data() + values.size());
    REQUIRE(view.end() - view.begin() == static_cast<std::ptrdiff_t>(view.size()));
    REQUIRE(const_view.begin() == values.data());
    REQUIRE(const_view.end() == values.data() + values.size());
    REQUIRE(sum == 46);
    REQUIRE(values == std::array<std::int16_t, 4>{10, 11, 12, 13});
}

TEST_CASE("tensor view accepts explicit shapes over spans", "[tensor_view]") {
    auto values = std::to_array<std::int16_t>({1, 2, 3, 4});
    std::span<std::int16_t> values_span(values);
    tensor_view matrix_view(values_span, tensor_shape{2, 2},
                            domain::no_quantization<std::int16_t>{});

    REQUIRE(matrix_view.data() == values.data());
    REQUIRE(matrix_view.shape() == tensor_shape{2, 2});
    REQUIRE(matrix_view.size() == 4);
    REQUIRE(matrix_view[1, 0] == 3);

    matrix_view[1, 0] = 7;
    REQUIRE(values[2] == 7);
}

TEST_CASE("tensor view deduces no-quantization domain from explicit shapes", "[tensor_view]") {
    auto values = std::to_array<std::int16_t>({1, 2, 3, 4});

    tensor_view matrix_view(values, tensor_shape{2, 2});

    STATIC_REQUIRE(
        std::same_as<decltype(matrix_view)::domain_type, domain::no_quantization<std::int16_t>>);
    REQUIRE(matrix_view.shape() == tensor_shape{2, 2});
    REQUIRE(matrix_view[1, 0] == 3);
}

TEST_CASE("tensor view preserves const storage from contiguous sources", "[tensor_view]") {
    constexpr auto values = std::to_array<std::int16_t>({1, 2, 3, 4});

    tensor_view view(values, tensor_shape{2, 2});

    STATIC_REQUIRE(
        std::same_as<decltype(view)::domain_type, domain::no_quantization<std::int16_t>>);
    STATIC_REQUIRE(std::same_as<decltype(view)::element_type, const std::int16_t>);
    STATIC_REQUIRE(std::same_as<decltype(view.data()), const std::int16_t*>);
    STATIC_REQUIRE(std::same_as<decltype(view.begin()), const std::int16_t*>);
    STATIC_REQUIRE(std::same_as<decltype(view[0, 0]), const std::int16_t&>);
    REQUIRE(view.data() == values.data());
    REQUIRE(view.shape() == tensor_shape{2, 2});
    REQUIRE(view[1, 0] == 3);
}

TEST_CASE("tensor view can be constructed as a constant expression", "[tensor_view]") {
    STATIC_REQUIRE(
        std::same_as<decltype(constexpr_view)::domain_type, domain::no_quantization<float>>);
    STATIC_REQUIRE(std::same_as<decltype(constexpr_view)::shape_type, tensor_shape<2>>);
    STATIC_REQUIRE(std::same_as<decltype(constexpr_view)::element_type, const float>);
    STATIC_REQUIRE(constexpr_view.rank() == 2);
    STATIC_REQUIRE(constexpr_view.size() == 4);
    STATIC_REQUIRE(constexpr_view.shape() == constexpr_view_shape);
    STATIC_REQUIRE(constexpr_view.data()[0] == 1.0F);
    STATIC_REQUIRE(constexpr_view.data()[3] == 4.0F);
    STATIC_REQUIRE(
        std::same_as<decltype(constexpr_view.domain()), const domain::no_quantization<float>&>);
}

TEST_CASE("tensor view preserves zero extent semantics", "[tensor_view]") {
    std::array<std::int16_t, 1> values = {42};
    tensor_view<std::int16_t, domain::no_quantization<std::int16_t>, tensor_shape<2>> empty(
        values, tensor_shape{0, 3});

    REQUIRE(empty.shape() == tensor_shape{0, 3});
    REQUIRE(empty.empty());
}

TEST_CASE("tensor view constructs from tensor owners", "[tensor_view]") {
    tensor values(tensor_shape{2, 2}, domain::no_quantization<std::int16_t>{});
    values.data()[0] = 1;
    values.data()[1] = 2;
    values.data()[2] = 3;
    values.data()[3] = 4;

    tensor_view view(values);

    STATIC_REQUIRE(
        std::same_as<decltype(view)::domain_type, domain::no_quantization<std::int16_t>>);
    REQUIRE(view.data() == values.data());
    REQUIRE(view.shape() == values.shape());
    REQUIRE(view[1, 0] == 3);
}

TEST_CASE("tensor view constructs from compatible tensor-like sources", "[tensor_view]") {
    auto values = std::to_array<std::int16_t>({1, 2, 3, 4});
    calibrated_tensor_like source{
        .storage = values.data(),
        .logical_size = values.size(),
        .logical_shape = tensor_shape{2, 2},
        .logical_domain = calibration_domain{.profile = 11},
    };

    tensor_view view(source);

    REQUIRE(view.data() == values.data());
    REQUIRE(view.shape() == source.shape());
    REQUIRE(view.size() == values.size());
    REQUIRE(view.domain().profile == 11);
    REQUIRE(view[1, 1] == 4);
}

TEST_CASE("tensor view accepts standard contiguous containers", "[tensor_view]") {
    std::vector<std::int16_t> values = {1, 2, 3, 4};
    tensor_view view(values, tensor_shape{2, 2}, domain::no_quantization<std::int16_t>{});

    REQUIRE(view.data() == values.data());
    REQUIRE(view.shape() == tensor_shape{2, 2});
    REQUIRE(view[1, 1] == 4);
}

TEST_CASE("tensor view rejects ranges smaller than the requested shape", "[tensor_view]") {
    std::array<std::int16_t, 3> values = {1, 2, 3};

    REQUIRE_THROWS_MATCHES(
        tensor_view(values, tensor_shape{2, 2}, domain::no_quantization<std::int16_t>{}),
        std::invalid_argument,
        Catch::Matchers::Message(
            "tensor_view source range is smaller than the logical tensor shape"));
}

TEST_CASE("tensor view rejects tensor-like sources smaller than the requested shape",
          "[tensor_view]") {
    std::array<std::int16_t, 3> values = {1, 2, 3};
    broken_tensor_like source{
        .storage = values.data(),
        .logical_size = values.size(),
        .logical_shape = tensor_shape{2, 2},
        .logical_domain = {},
    };

    REQUIRE_THROWS_MATCHES(
        tensor_view(source), std::invalid_argument,
        Catch::Matchers::Message(
            "tensor_view source range is smaller than the logical tensor shape"));
}
