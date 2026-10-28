// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0
#include "esp_memory_block_pool_resource.hpp"

#include <algorithm>
#include <atomic>
#include <array>
#include <concepts>
#include <cstdlib>
#include <latch>
#include <limits>
#include <thread>
#include <type_traits>
#include <vector>
#include <catch2/catch_test_macros.hpp>

using esp::memory::block_size;
using esp::memory::number_of_blocks;

static_assert(!std::is_constructible_v<esp::memory::block_pool_resource, std::size_t, std::size_t,
                                       std::size_t, std::pmr::memory_resource&>);
static_assert(!std::is_constructible_v<esp::memory::block_pool_resource, number_of_blocks,
                                       block_size, std::size_t, std::pmr::memory_resource&>);

namespace {
class counting_resource final : public std::pmr::memory_resource {
public:
    struct allocation {
        void* address;
        std::size_t bytes;
        std::size_t alignment;
    };
    unsigned allocations = 0;
    unsigned deallocations = 0;
    unsigned max_allocations = std::numeric_limits<unsigned>::max();
    std::vector<allocation> requests;
    bool matching_returns = true;

private:
    void* do_allocate(std::size_t bytes, std::size_t alignment) override {
        if (allocations == max_allocations) {
#if defined(__cpp_exceptions)
            throw std::bad_alloc{};
#else
            std::abort();
#endif
        }
        ++allocations;
        auto* address = std::pmr::new_delete_resource()->allocate(bytes, alignment);
        requests.push_back({address, bytes, alignment});
        return address;
    }
    void do_deallocate(void* address, std::size_t bytes, std::size_t alignment) override {
        ++deallocations;
        const auto found =
            std::find_if(requests.begin(), requests.end(),
                         [address](const auto& request) { return request.address == address; });
        if (found == requests.end() || found->bytes != bytes || found->alignment != alignment) {
            matching_returns = false;
        }
        std::pmr::new_delete_resource()->deallocate(address, bytes, alignment);
    }
    bool do_is_equal(const std::pmr::memory_resource& other) const noexcept override {
        return this == &other;
    }
};
} // namespace

TEST_CASE("fixed block pool returns aligned pointers and reuses out-of-order blocks",
          "[memory][pool]") {
    counting_resource upstream;
    {
        esp::memory::block_pool_resource pool{block_size{13}, number_of_blocks{4}, 16, upstream};
        std::array<std::byte*, 4> held{};
        for (auto& pointer : held) {
            pointer = static_cast<std::byte*>(pool.allocate(13, 16));
            REQUIRE(reinterpret_cast<std::uintptr_t>(pointer) % 16 == 0);
        }
#if defined(__cpp_exceptions)
        REQUIRE_THROWS_AS(pool.allocate(13, 16), std::bad_alloc);
        REQUIRE(upstream.allocations == 4);
#endif
        auto* address = held[3];
        pool.deallocate(held[3], 13, 16);
        auto* replacement = static_cast<std::byte*>(pool.allocate(13, 16));
        REQUIRE(replacement == address);
        for (std::size_t index = 0; index < 3; ++index) {
            pool.deallocate(held[index], 13, 16);
        }
        pool.deallocate(replacement, 13, 16);
        REQUIRE(upstream.allocations == 4);
        for (const auto& request : upstream.requests) {
            REQUIRE(request.bytes == 13);
            REQUIRE(request.alignment == 16);
        }
    }
    REQUIRE(upstream.deallocations == 4);
    REQUIRE(upstream.matching_returns);
}

TEST_CASE("fixed block pool uses a bounded arena with an unaligned start",
          "[memory][pool][static]") {
    alignas(64) std::array<std::byte, 4 * 64 + 64> storage{};
    std::pmr::monotonic_buffer_resource arena{storage.data() + 1, storage.size() - 1,
                                              std::pmr::null_memory_resource()};
    esp::memory::block_pool_resource pool{block_size{13}, number_of_blocks{4}, 64, arena};
    std::array<std::byte*, 4> held{};
    for (auto& pointer : held) {
        pointer = static_cast<std::byte*>(pool.allocate(13, 64));
        REQUIRE(reinterpret_cast<std::uintptr_t>(pointer) % 64 == 0);
        REQUIRE(pointer > storage.data());
        REQUIRE(pointer + 13 <= storage.data() + storage.size());
    }
    for (auto* pointer : held) {
        pool.deallocate(pointer, 13, 64);
    }
}

TEST_CASE("fixed block pool supports concurrent raw-pointer checkout",
          "[memory][pool][concurrency]") {
    esp::memory::block_pool_resource pool{block_size{32}, number_of_blocks{4}, 16,
                                          *std::pmr::new_delete_resource()};
    std::array<void*, 4> addresses{};
    for (auto& address : addresses) {
        address = pool.allocate(32, 16);
    }
    for (auto* address : addresses) {
        pool.deallocate(address, 32, 16);
    }
    std::array<std::atomic_bool, 4> active{};
    std::atomic_bool distinct = true;
    std::latch start{3};
    const auto use_pool = [&]() {
        start.arrive_and_wait();
        for (unsigned progress = 0; progress < 1000;) {
            auto* pointer = pool.allocate(32, 16);
            const auto found = std::find(addresses.begin(), addresses.end(), pointer);
            if (found == addresses.end()) {
                distinct.store(false);
                pool.deallocate(pointer, 32, 16);
                continue;
            }
            auto& busy = active[static_cast<std::size_t>(found - addresses.begin())];
            if (busy.exchange(true)) {
                distinct.store(false);
            }
            std::this_thread::yield();
            busy.store(false);
            pool.deallocate(pointer, 32, 16);
            ++progress;
        }
    };
    std::thread first{use_pool};
    std::thread second{use_pool};
    start.arrive_and_wait();
    first.join();
    second.join();
    REQUIRE(distinct.load());
}

#if defined(__cpp_exceptions)
namespace {
[[noreturn]] void capture_failure(const esp::memory::pool_failure& failure) { throw failure; }

void expect_failure(std::invocable auto&& operation, esp::memory::pool_failure expected) {
    try {
        operation();
        FAIL("Expected the pool failure handler to throw");
    } catch (const esp::memory::pool_failure& failure) {
        REQUIRE(failure.reason == expected.reason);
        REQUIRE(failure.bytes == expected.bytes);
        REQUIRE(failure.alignment == expected.alignment);
    }
}
} // namespace
TEST_CASE("two users do not exhaust a pool with one block always free",
          "[memory][pool][concurrency]") {
    esp::memory::block_pool_resource pool{block_size{32}, number_of_blocks{2}, 16,
                                          *std::pmr::new_delete_resource(), capture_failure};
    auto* first_block = pool.allocate(32, 16);
    auto* second_block = pool.allocate(32, 16);
    std::atomic_bool failed = false;
    std::latch start{3};
    const auto use_pool = [&](void* held) {
        start.arrive_and_wait();
        for (unsigned iteration = 0; iteration < 10000; ++iteration) {
            pool.deallocate(held, 32, 16);
            std::this_thread::yield();
            try {
                held = pool.allocate(32, 16);
            } catch (const esp::memory::pool_failure&) {
                failed.store(true);
                return;
            }
        }
        pool.deallocate(held, 32, 16);
    };
    std::thread first{use_pool, first_block};
    std::thread second{use_pool, second_block};
    start.arrive_and_wait();
    first.join();
    second.join();
    REQUIRE_FALSE(failed.load());
}

TEST_CASE("block pool supports all 32 availability bits", "[memory][pool][limit]") {
    esp::memory::block_pool_resource pool{block_size{32}, number_of_blocks{32}, 16,
                                          *std::pmr::new_delete_resource(), capture_failure};
    std::array<void*, 32> blocks{};
    for (auto& block : blocks) {
        block = pool.allocate(32, 16);
        REQUIRE(std::find(blocks.begin(), &block, block) == &block);
    }
    expect_failure([&] { (void)pool.allocate(32, 16); },
                   {esp::memory::pool_failure_reason::exhausted, 32, 16});
    pool.deallocate(blocks.back(), 32, 16);
    REQUIRE(pool.allocate(32, 16) == blocks.back());
    for (auto* block : blocks) {
        pool.deallocate(block, 32, 16);
    }
}

TEST_CASE("block pool construction reports unavailable initial storage",
          "[memory][pool][failure]") {
    using enum esp::memory::pool_failure_reason;
    expect_failure(
        [] {
            esp::memory::block_pool_resource pool{block_size{13}, number_of_blocks{2}, 16,
                                                  *std::pmr::null_memory_resource(),
                                                  capture_failure};
        },
        {upstream_allocation, 13, 16});

    std::array<std::byte, 31> too_small{};
    std::pmr::monotonic_buffer_resource arena{too_small.data(), too_small.size(),
                                              std::pmr::null_memory_resource()};
    expect_failure(
        [&] {
            esp::memory::block_pool_resource pool{block_size{32}, number_of_blocks{1}, 16, arena,
                                                  capture_failure};
        },
        {upstream_allocation, 32, 16});
}

TEST_CASE("block pool returns earlier blocks if construction fails", "[memory][pool][failure]") {
    counting_resource upstream;
    upstream.max_allocations = 2;
    expect_failure(
        [&] {
            esp::memory::block_pool_resource pool{block_size{13}, number_of_blocks{4}, 16, upstream,
                                                  capture_failure};
        },
        {esp::memory::pool_failure_reason::upstream_allocation, 13, 16});
    REQUIRE(upstream.allocations == 2);
    REQUIRE(upstream.deallocations == 2);
    REQUIRE(upstream.matching_returns);
}

TEST_CASE("block pool reports rejected allocation requests", "[memory][pool][failure]") {
    using enum esp::memory::pool_failure_reason;
    esp::memory::block_pool_resource bounded{block_size{32}, number_of_blocks{1}, 16,
                                             *std::pmr::new_delete_resource(), capture_failure};
    auto* held = bounded.allocate(32, 16);
    expect_failure([&] { (void)bounded.allocate(13, 8); }, {exhausted, 13, 8});
    bounded.deallocate(held, 32, 16);
    expect_failure([&] { (void)bounded.allocate(64, 16); }, {unsupported_request, 64, 16});
    expect_failure([&] { (void)bounded.allocate(32, 32); }, {unsupported_request, 32, 32});
}

#endif
