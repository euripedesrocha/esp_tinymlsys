#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_heap_caps.h"
#include "esp_memory_resource.hpp"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "sdkconfig.h"

namespace {

constexpr bool is_aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
}

struct alignas(64) dma_block {
    std::array<std::byte, 96> bytes{};
};

} // namespace

TEST_CASE("pmr::vector<std::byte> uses DMA-capable internal storage", "[alloc]") {
    using esp::memory::capabilities;

    esp::memory::resource resource(capabilities::dma | capabilities::internal);
    std::pmr::vector<std::byte> data(&resource);

    data.reserve(1536);
    data.resize(1536);

    REQUIRE(data.data() != nullptr);
    REQUIRE(esp_ptr_dma_capable(data.data()));
    REQUIRE(esp_ptr_internal(data.data()));
    REQUIRE(esp_ptr_byte_accessible(data.data()));
    REQUIRE(heap_caps_get_allocated_size(data.data()) >= data.size());
}

TEST_CASE("pmr::vector<std::byte> can enforce accelerator-friendly alignment", "[align]") {
    using esp::memory::capabilities;

    esp::memory::accelerator_resource resource(capabilities::dma | capabilities::internal |
                                               esp::memory::accelerator_capabilities());
    std::pmr::vector<std::byte> data(&resource);

    data.resize(1024);

    REQUIRE(data.data() != nullptr);
    REQUIRE(is_aligned(data.data(), esp::memory::accelerator_resource::alignment));
    REQUIRE(esp_ptr_dma_capable(data.data()));
    REQUIRE(esp_ptr_internal(data.data()));
}

TEST_CASE("pmr::vector keeps constrained storage after growth", "[alloc]") {
    using esp::memory::capabilities;

    esp::memory::accelerator_resource resource(capabilities::dma | capabilities::internal |
                                               esp::memory::accelerator_capabilities());
    std::pmr::vector<std::byte> data(&resource);

    data.resize(256);
    auto* first_ptr = data.data();

    data.reserve(4096);
    data.resize(4096);

    REQUIRE(data.data() != nullptr);
    REQUIRE(data.data() != first_ptr);
    REQUIRE(is_aligned(data.data(), esp::memory::accelerator_resource::alignment));
    REQUIRE(esp_ptr_dma_capable(data.data()));
    REQUIRE(esp_ptr_internal(data.data()));
    REQUIRE(esp_ptr_byte_accessible(data.data()));
}

TEST_CASE("pmr::vector of over-aligned elements preserves alignment", "[align]") {
    using esp::memory::capabilities;

    esp::memory::resource resource(capabilities::dma | capabilities::internal);
    std::pmr::vector<dma_block> data(&resource);

    data.resize(4);

    REQUIRE(data.data() != nullptr);
    REQUIRE(is_aligned(data.data(), alignof(dma_block)));
    REQUIRE(esp_ptr_dma_capable(data.data()));
    REQUIRE(esp_ptr_internal(data.data()));
}

TEST_CASE("pmr::string stores content in byte-addressable internal memory", "[alloc]") {
    using esp::memory::capabilities;

    esp::memory::resource resource(capabilities::internal | capabilities::addressable_8bit);
    std::pmr::string text(&resource);

    text.reserve(256);
    text.assign(200, 'm');

    REQUIRE(text.data() != nullptr);
    REQUIRE(text.size() == 200);
    REQUIRE(esp_ptr_internal(text.data()));
    REQUIRE(esp_ptr_byte_accessible(text.data()));
}

TEST_CASE("resource accessors reflect configured capabilities and alignment", "[accessor]") {
    using esp::memory::capabilities;

    esp::memory::resource basic(capabilities::dma | capabilities::internal);
    esp::memory::accelerator_resource accel(capabilities::dma | capabilities::internal |
                                            esp::memory::accelerator_capabilities());

    REQUIRE(basic.caps() == (capabilities::dma | capabilities::internal));
    REQUIRE(basic.minimum_alignment() == 1);
    REQUIRE(accel.caps() ==
            (capabilities::dma | capabilities::internal | esp::memory::accelerator_capabilities()));
    REQUIRE(accel.minimum_alignment() == esp::memory::accelerator_resource::alignment);
}

TEST_CASE("pmr::vector of strongly aligned elements preserves stronger alignment", "[align]") {
    using esp::memory::capabilities;

    struct alignas(128) wider_block {
        std::array<std::byte, 128> bytes{};
    };

    esp::memory::accelerator_resource resource(capabilities::dma | capabilities::internal |
                                               esp::memory::accelerator_capabilities());
    std::pmr::vector<wider_block> data(&resource);

    data.resize(2);

    REQUIRE(data.data() != nullptr);
    REQUIRE(is_aligned(data.data(), alignof(wider_block)));
    REQUIRE(alignof(wider_block) > esp::memory::accelerator_resource::alignment);
    REQUIRE(esp_ptr_dma_capable(data.data()));
    REQUIRE(esp_ptr_internal(data.data()));
}

TEST_CASE("pmr::vector throws std::bad_alloc on impossible allocation", "[failure]") {
    esp::memory::resource resource(esp::memory::capabilities::internal);
    std::pmr::vector<std::byte> data(&resource);

    REQUIRE_THROWS_AS(data.reserve(SIZE_MAX / 2), std::bad_alloc);
}

#if CONFIG_SPIRAM
TEST_CASE("pmr::vector can be placed in SPIRAM when available", "[spiram]") {
    if (!esp_psram_is_initialized()) {
        SKIP("PSRAM is not initialized on this target");
    }

    esp::memory::resource resource(esp::memory::capabilities::from_spiram);
    std::pmr::vector<std::byte> data(&resource);

    data.resize(2048);

    REQUIRE(data.data() != nullptr);
    REQUIRE(esp_ptr_external_ram(data.data()));
    REQUIRE(esp_ptr_byte_accessible(data.data()));
}
#endif
