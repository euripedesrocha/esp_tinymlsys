#include <catch2/catch_test_macros.hpp>
#include <concepts>
#include <cstdint>
#include <cstddef>
#include <memory_resource>

#include "esp_tensor.hpp"
#include "esp_tensor_domain.hpp"

using namespace esp::tensor;

namespace {

struct test_domain {
    using value_type = std::int8_t;
};

class counting_resource : public std::pmr::memory_resource {
public:
    std::size_t allocations = 0;
    std::size_t deallocations = 0;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        ++allocations;
        return std::pmr::new_delete_resource()->allocate(bytes, alignment);
    }

    void do_deallocate(void* pointer, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        std::pmr::new_delete_resource()->deallocate(pointer, bytes, alignment);
    }

    [[nodiscard]] bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
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
    float sum = 0;
    for (const auto value : const_values) {
        sum += value;
    }

    STATIC_REQUIRE(decltype(values)::rank() == 2);
    STATIC_REQUIRE(std::same_as<typename decltype(values)::domain_type::value_type, float>);
    REQUIRE(values.size() == 4);
    REQUIRE(values.data()[0] == 1.0F);
    REQUIRE(values.data()[3] == 4.0F);
    REQUIRE(values.shape() == tensor_shape{2, 2});
    REQUIRE(sum == 10.0F);
}

TEST_CASE("tensor supports multidimensional indexing", "[tensor]") {
    tensor values(tensor_shape{2, 2}, domain::no_quantization<float>{});

    values.data()[0] = 1.0F;
    values.data()[1] = 2.0F;
    values.data()[2] = 3.0F;
    values.data()[3] = 4.0F;

    REQUIRE(values[0, 0] == 1.0F);
    REQUIRE(values[0, 1] == 2.0F);
    REQUIRE(values[1, 0] == 3.0F);
    REQUIRE(values[1, 1] == 4.0F);
}

TEST_CASE("tensor preserves zero extent semantics", "[tensor]") {
    tensor values(tensor_shape{0, 3});

    REQUIRE(values.empty());
    REQUIRE(values.shape() == tensor_shape{0, 3});
}

TEST_CASE("tensor copy construction preserves source memory resource", "[tensor]") {
    counting_resource resource;
    tensor values(tensor_shape{2, 2}, test_domain{}, &resource);
    values.data()[0] = 1;
    values.data()[3] = 4;

    const auto allocations_before_copy = resource.allocations;
    tensor copy(values);

    REQUIRE(resource.allocations == allocations_before_copy + 1);
    REQUIRE(copy.size() == values.size());
    REQUIRE(copy.shape() == values.shape());
    REQUIRE(copy.data()[0] == 1);
    REQUIRE(copy.data()[3] == 4);
}

TEST_CASE("tensor copy assignment preserves source memory resource", "[tensor]") {
    counting_resource source_resource;
    counting_resource destination_resource;
    tensor source(tensor_shape{2, 2}, test_domain{}, &source_resource);
    tensor destination(tensor_shape{1, 1}, test_domain{}, &destination_resource);

    source.data()[0] = 7;
    source.data()[3] = 9;
    const auto source_allocations_before_copy = source_resource.allocations;
    destination = source;

    REQUIRE(source_resource.allocations == source_allocations_before_copy + 1);
    REQUIRE(destination.size() == source.size());
    REQUIRE(destination.shape() == source.shape());
    REQUIRE(destination.data()[0] == 7);
    REQUIRE(destination.data()[3] == 9);
}
