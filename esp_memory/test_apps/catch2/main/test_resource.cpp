#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "esp_heap_caps.h"
#include "esp_memory_block_pool_resource.hpp"
#include "esp_memory_resource.hpp"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_private/heap_align_hw.h"
#include "sdkconfig.h"

static_assert(!std::is_constructible_v<esp::memory::block_pool_resource, std::size_t, std::size_t,
                                       esp::memory::resource&>);
static_assert(
    !std::is_constructible_v<esp::memory::block_pool_resource, esp::memory::number_of_blocks,
                             esp::memory::block_size, esp::memory::resource&>);

namespace {

constexpr bool is_aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) % alignment) == 0;
}

struct alignas(64) dma_block {
    std::array<std::byte, 96> bytes{};
};

std::size_t idf_alignment(esp::memory::capabilities capabilities, std::size_t alignment = 1) {
    alignment = std::max(alignment, alignof(std::uint32_t));
    auto caps = esp::memory::get_caps(capabilities);
    std::size_t size = 1;
    esp_heap_adjust_alignment_to_hw(&alignment, &size, &caps);
    REQUIRE((caps & static_cast<std::uint32_t>(MALLOC_CAP_INVALID)) == 0);
    return alignment;
}

void check_dma_blocks(esp::memory::block_pool_resource& pool, std::size_t alignment,
                      std::size_t requested_alignment, std::size_t count) {
    std::array<std::byte*, 7> blocks{};
    for (std::size_t index = 0; index < count; ++index) {
        auto* pointer = static_cast<std::byte*>(pool.allocate(13, requested_alignment));
        blocks[index] = pointer;
        REQUIRE(is_aligned(pointer, alignment));
        REQUIRE(esp_ptr_dma_capable(pointer));
        REQUIRE(esp_ptr_internal(pointer));
        REQUIRE(esp_ptr_byte_accessible(pointer));
        REQUIRE(esp_ptr_dma_capable(pointer + 12));
        REQUIRE(esp_ptr_internal(pointer + 12));
        pointer[0] = std::byte{42};
        pointer[12] = std::byte{24};
    }
    for (std::size_t index = 0; index < count; ++index) {
        auto* pointer = blocks[index];
        REQUIRE(pointer[0] == std::byte{42});
        REQUIRE(pointer[12] == std::byte{24});
        pool.deallocate(pointer, 13, requested_alignment);
    }
}

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
    REQUIRE(basic.minimum_alignment() == idf_alignment(basic.caps()));
    REQUIRE(accel.caps() ==
            (capabilities::dma | capabilities::internal | esp::memory::accelerator_capabilities()));
    REQUIRE(accel.minimum_alignment() ==
            idf_alignment(accel.caps(), esp::memory::accelerator_resource::alignment));
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

TEST_CASE("block pool infers capability alignment for every block", "[memory][pool][dma][align]") {
    using esp::memory::capabilities;
    esp::memory::resource upstream{capabilities::dma | capabilities::internal};
    esp::memory::accelerator_resource accelerator{capabilities::dma | capabilities::internal |
                                                  esp::memory::accelerator_capabilities()};
    for (auto* source : {&upstream, static_cast<esp::memory::resource*>(&accelerator)}) {
        const auto alignment = std::max(alignof(std::max_align_t), source->minimum_alignment());
        esp::memory::block_pool_resource pool{esp::memory::block_size{13},
                                              esp::memory::number_of_blocks{3}, *source};
        check_dma_blocks(pool, alignment, alignof(std::max_align_t), 3);
        check_dma_blocks(pool, alignment, alignof(std::max_align_t), 3);
    }
}

TEST_CASE("block pool raises weak alignment and preserves stronger requests",
          "[memory][pool][dma][align]") {
    using esp::memory::capabilities;
    esp::memory::resource basic{capabilities::dma | capabilities::internal};
    esp::memory::accelerator_resource accelerator{capabilities::dma | capabilities::internal |
                                                  esp::memory::accelerator_capabilities()};
    for (auto* source : {&basic, static_cast<esp::memory::resource*>(&accelerator)}) {
        const auto minimum = source->minimum_alignment();
        REQUIRE(minimum > 1);
        for (auto requested : {std::size_t{1}, minimum * 2}) {
            esp::memory::block_pool_resource pool{
                esp::memory::block_size{13}, esp::memory::number_of_blocks{3}, requested, *source};
            check_dma_blocks(pool, std::max(minimum, requested), requested, 3);
            check_dma_blocks(pool, std::max(minimum, requested), requested, 3);
        }
    }
}

TEST_CASE("resource rejects invalid hardware capability requirements", "[memory][failure]") {
    esp::memory::resource source{static_cast<esp::memory::capabilities>(MALLOC_CAP_INVALID)};
    REQUIRE_THROWS_AS(source.minimum_alignment(), std::bad_alloc);
    REQUIRE_THROWS_AS(source.allocate(13, 1), std::bad_alloc);
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

TEST_CASE("fixed pool buffers use PSRAM backing when requested", "[spiram][pool]") {
    if (!esp_psram_is_initialized()) {
        SKIP("PSRAM is not initialized on this target");
    }
    esp::memory::resource upstream(esp::memory::capabilities::from_spiram);
    // The pool object itself can also live in PSRAM. Its atomic availability
    // mask must therefore have placement independent of the object's address.
    using pool_type = esp::memory::block_pool_resource;
    std::pmr::polymorphic_allocator<pool_type> allocator{&upstream};
    const auto destroy = [&](pool_type* pool) { allocator.delete_object(pool); };
    std::unique_ptr<pool_type, decltype(destroy)> fixed{
        allocator.new_object<pool_type>(esp::memory::block_size{13},
                                        esp::memory::number_of_blocks{2}, upstream),
        destroy};
    const auto alignment = std::max(alignof(std::max_align_t), upstream.minimum_alignment());
    REQUIRE(esp_ptr_external_ram(fixed.get()));
    std::array<std::byte*, 2> buffers{};
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        buffers[index] = static_cast<std::byte*>(fixed->allocate(13));
        buffers[index][0] = static_cast<std::byte>(index);
        REQUIRE(esp_ptr_external_ram(buffers[index]));
        REQUIRE(esp_ptr_external_ram(buffers[index] + 12));
        REQUIRE(is_aligned(buffers[index], alignment));
    }
    for (std::size_t index = 0; index < buffers.size(); ++index) {
        REQUIRE(buffers[index][0] == static_cast<std::byte>(index));
        fixed->deallocate(buffers[index], 13);
    }
}
#endif
