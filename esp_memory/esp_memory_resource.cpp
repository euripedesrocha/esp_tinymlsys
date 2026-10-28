#include "esp_memory_resource.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdlib>
#include <new>
#include "esp_log.h"
#include "esp_private/heap_align_hw.h"

namespace esp::memory {

std::size_t resource::minimum_alignment() const {
    // IDF's heap provides word alignment even when hardware adds no stricter requirement.
    auto alignment = std::max(min_alignment_, alignof(std::uint32_t));
    std::size_t size = 1;
    auto caps = get_caps(caps_);
    esp_heap_adjust_alignment_to_hw(&alignment, &size, &caps);
    if ((caps & static_cast<std::uint32_t>(MALLOC_CAP_INVALID)) != 0) {
#if defined(__cpp_exceptions)
        throw std::bad_alloc{};
#else
        std::abort();
#endif
    }
    return alignment;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* resource::do_allocate(size_t bytes, size_t alignment) {
    const auto effective = detail::effective_alignment(alignment, minimum_alignment());

    if (void* mem = heap_caps_aligned_alloc(effective, bytes, get_caps(caps_)); mem != nullptr) {
        ESP_LOGD(tag, "Allocated %zu bytes at %p with alignment=%zu", bytes, mem, effective);
        return mem;
    }

    ESP_LOGD(tag, "Failed to allocate %zu bytes with caps=0x%" PRIx32 " alignment=%zu", bytes,
             get_caps(caps_), effective);
#if defined(__cpp_exceptions)
    throw std::bad_alloc{};
#else
    std::abort(); // Strict failure when the application disables C++ exceptions.
#endif
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void resource::do_deallocate(void* ptr, [[maybe_unused]] size_t bytes,
                             [[maybe_unused]] size_t alignment) {
    std::free(ptr);
    ESP_LOGD(tag, "Freed %p", ptr);
}

bool resource::do_is_equal(const memory_resource& other) const noexcept { return this == &other; }

} // namespace esp::memory
