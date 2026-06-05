#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <memory_resource>

#include "esp_tensor.hpp"
#include "esp_tensor_domain.hpp"

using namespace esp::tensor;

namespace {

struct test_domain {
    using value_type = std::int8_t;
};

} // namespace

TEST_CASE("tensor defaults to vector of float when constructed from shape", "[tensor]") {
    tensor values(tensor_shape{2, 3});

    STATIC_REQUIRE(std::same_as<decltype(values)::shape_type, tensor_shape<2>>);
    STATIC_REQUIRE(std::same_as<decltype(values)::domain_type, domain::no_quantization<>>);
    STATIC_REQUIRE(std::same_as<typename decltype(values)::domain_type::value_type, float>);
    REQUIRE(values.size() == 6);
}

TEST_CASE("tensor accepts explicit domain with default resource", "[tensor]") {
    tensor values(tensor_shape{2, 3}, test_domain{});

    STATIC_REQUIRE(std::same_as<decltype(values)::domain_type, test_domain>);
    STATIC_REQUIRE(std::same_as<typename decltype(values)::domain_type::value_type, std::int8_t>);
    REQUIRE(values.size() == 6);
}

TEST_CASE("tensor accepts explicit memory resource with domain-aware construction", "[tensor]") {
    std::pmr::monotonic_buffer_resource resource;
    tensor values(tensor_shape{2, 2}, test_domain{}, &resource);

    STATIC_REQUIRE(std::same_as<decltype(values)::shape_type, tensor_shape<2>>);
    STATIC_REQUIRE(std::same_as<decltype(values)::domain_type, test_domain>);
    STATIC_REQUIRE(std::same_as<typename decltype(values)::domain_type::value_type, std::int8_t>);
    REQUIRE(values.size() == 4);
    REQUIRE(values.shape() == tensor_shape{2, 2});
}

TEST_CASE("tensor supports range based fill and iteration", "[tensor]") {
    tensor values(tensor_shape{2, 2});

    float next = 1;
    for (auto& value : values) {
        value = next++;
    }

    const decltype(values)& const_values = values;
    std::int32_t sum = 0;
    for (const auto value : const_values) {
        sum += value;
    }

    STATIC_REQUIRE(decltype(values)::rank() == 2);
    STATIC_REQUIRE(std::same_as<typename decltype(values)::domain_type::value_type, float>);
    REQUIRE(values.size() == 4);
    REQUIRE(values.data()[0] == 1);
    REQUIRE(values.data()[3] == 4);
    REQUIRE(values.shape() == tensor_shape{2, 2});
    REQUIRE(sum == 10);
}

TEST_CASE("tensor supports multidimensional indexing", "[tensor]") {
    tensor values(tensor_shape{2, 2}, domain::no_quantization<std::int16_t>{});

    values.data()[0] = 1;
    values.data()[1] = 2;
    values.data()[2] = 3;
    values.data()[3] = 4;

    REQUIRE(values[0, 0] == 1);
    REQUIRE(values[0, 1] == 2);
    REQUIRE(values[1, 0] == 3);
    REQUIRE(values[1, 1] == 4);
}

TEST_CASE("tensor preserves zero extent semantics", "[tensor]") {
    tensor values(tensor_shape{0, 3});

    REQUIRE(values.size() == 0);
    REQUIRE(values.empty());
    REQUIRE(values.shape() == tensor_shape{0, 3});
}
