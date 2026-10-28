// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <algorithm>
#include <atomic>
#include <bit>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <memory_resource>
#include <new>
#include <utility>
#include <vector>

#if defined(ESP_PLATFORM)
#include "esp_memory_resource.hpp"
#include "sdkconfig.h"
#endif

namespace esp::memory {

enum class pool_failure_reason : uint8_t { exhausted, unsupported_request, upstream_allocation };

struct pool_failure {
    pool_failure_reason reason;
    std::size_t bytes;
    std::size_t alignment;
};

using pool_failure_handler = void (*)(const pool_failure&);

enum class block_size : std::size_t {};
enum class number_of_blocks : std::size_t {};

[[noreturn]] inline void allocation_failure(const pool_failure&) {
#if defined(__cpp_exceptions)
    throw std::bad_alloc{};
#else
    std::abort();
#endif
}

class block_pool_resource final : public std::pmr::memory_resource {
#if !defined(ESP_PLATFORM) || !CONFIG_STDATOMIC_S32C1I_SPIRAM_WORKAROUND
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
#endif
    static std::size_t checked_count(std::size_t count) noexcept {
        if (count == 0 || count > 32) {
            std::abort();
        }
        return count;
    }
    static constexpr std::uint32_t all_available(std::size_t count) noexcept {
        return std::numeric_limits<std::uint32_t>::max() >> (32 - count);
    }
#if defined(ESP_PLATFORM)
    static std::size_t pool_alignment(std::size_t requested, const resource& source) {
        assert(requested && (requested & (requested - 1)) == 0);
        return std::max(requested, source.minimum_alignment());
    }
#endif

public:
#if defined(ESP_PLATFORM)
    block_pool_resource(block_size size, number_of_blocks count, resource& source,
                        pool_failure_handler failure = allocation_failure)
        : block_pool_resource(size, count, alignof(std::max_align_t), source, failure) {}

    block_pool_resource(block_size size, number_of_blocks count, std::size_t alignment,
                        resource& source, pool_failure_handler failure = allocation_failure)
        : block_pool_resource(size, count, pool_alignment(alignment, source),
                              static_cast<std::pmr::memory_resource&>(source), failure) {}
#endif

    block_pool_resource(block_size size, number_of_blocks count, std::size_t alignment,
                        std::pmr::memory_resource& upstream,
                        pool_failure_handler failure = allocation_failure)
        : upstream_(upstream), block_bytes_(std::to_underlying(size)), alignment_(alignment),
          failure_(failure), blocks_(checked_count(std::to_underlying(count)), metadata_resource()),
          available_(1, metadata_resource()) {
        assert(failure);
        assert(std::to_underlying(size) && alignment && (alignment & (alignment - 1)) == 0);
#if defined(__cpp_exceptions)
        try {
            allocate_blocks();
        } catch (...) {
            release_blocks();
            throw;
        }
#else
        // Without unwinding, an upstream allocation failure must terminate.
        allocate_blocks();
#endif
        available_.front().store(all_available(blocks_.size()), std::memory_order_relaxed);
    }
    ~block_pool_resource() override {
        if (available_.front().load(std::memory_order_relaxed) != all_available(blocks_.size())) {
            std::abort(); // Destruction with a checked-out block violates the pool lifetime.
        }
        release_blocks();
    }
    block_pool_resource(const block_pool_resource&) = delete;
    block_pool_resource& operator=(const block_pool_resource&) = delete;

private:
    auto metadata_resource() noexcept -> std::pmr::memory_resource* {
#if defined(ESP_PLATFORM)
        return &metadata_;
#else
        return std::pmr::new_delete_resource();
#endif
    }
    [[noreturn]] void fail(pool_failure_reason reason, std::size_t bytes, std::size_t alignment) {
        failure_({reason, bytes, alignment});
        std::abort(); // A user failure handler must not return.
    }
    void allocate_blocks() {
        for (auto& block : blocks_) {
            block = allocate_block();
        }
    }
    void release_blocks() {
        for (auto* block : blocks_) {
            if (block != nullptr) {
                upstream_.deallocate(block, block_bytes_, alignment_);
            }
        }
    }
    auto allocate_block() -> std::byte* {
#if defined(__cpp_exceptions)
        try {
            return static_cast<std::byte*>(upstream_.allocate(block_bytes_, alignment_));
        } catch (const std::bad_alloc&) {
            fail(pool_failure_reason::upstream_allocation, block_bytes_, alignment_);
        }
#else
        return static_cast<std::byte*>(upstream_.allocate(block_bytes_, alignment_));
#endif
    }
    auto claim() noexcept -> std::byte* {
        auto mask = available_.front().load(std::memory_order_relaxed);
        while (mask != 0) {
            const auto index = std::countr_zero(mask);
            const auto bit = std::uint32_t{1} << index;
            if (available_.front().compare_exchange_weak(
                    mask, mask & ~bit, std::memory_order_acquire, std::memory_order_relaxed)) {
                return blocks_[static_cast<std::size_t>(index)];
            }
        }
        return nullptr;
    }
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (bytes > block_bytes_ || alignment > alignment_) {
            fail(pool_failure_reason::unsupported_request, bytes, alignment);
        }
        if (auto* pointer = claim(); pointer != nullptr) {
            return pointer;
        }
        fail(pool_failure_reason::exhausted, bytes, alignment);
    }
    // The parameter types and order are fixed by std::pmr::memory_resource.
    // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
    void do_deallocate(void* pointer, [[maybe_unused]] std::size_t bytes,
                       [[maybe_unused]] std::size_t alignment) noexcept override {
        const auto block = std::find(blocks_.begin(), blocks_.end(), pointer);
        if (block == blocks_.end()) {
            std::abort(); // Returning a pointer from another resource violates the PMR contract.
        }
        const auto index = static_cast<std::size_t>(block - blocks_.begin());
        const auto bit = std::uint32_t{1} << index;
        if ((available_.front().fetch_or(bit, std::memory_order_release) & bit) != 0) {
            std::abort(); // Returning an already available block violates the PMR contract.
        }
    }
    bool do_is_equal(const memory_resource& other) const noexcept override {
        return this == &other;
    }

    std::pmr::memory_resource& upstream_;
    std::size_t block_bytes_, alignment_;
    pool_failure_handler failure_;
#if defined(ESP_PLATFORM)
    resource metadata_{capabilities::internal | capabilities::addressable_8bit};
#endif
    std::pmr::vector<std::byte*> blocks_;
    std::pmr::vector<std::atomic<std::uint32_t>> available_;
};
} // namespace esp::memory
