#include "esp_memory_resource.hpp"

#include <cinttypes>
#include <cstdlib>
#include <new>
#include "esp_log.h"

namespace esp::memory {

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void* resource::do_allocate(size_t bytes, size_t alignment) {
    const auto effective = detail::effective_alignment(alignment, min_alignment_);

    if (void* mem = heap_caps_aligned_alloc(effective, bytes, get_caps(caps_)); mem != nullptr) {
        ESP_LOGD(tag, "Allocated %zu bytes at %p with alignment=%zu", bytes, mem, effective);
        return mem;
    }

    ESP_LOGD(tag, "Failed to allocate %zu bytes with caps=0x%" PRIx32 " alignment=%zu", bytes,
             get_caps(caps_), effective);
    throw std::bad_alloc{};
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void resource::do_deallocate(void* ptr, [[maybe_unused]] size_t bytes,
                             [[maybe_unused]] size_t alignment) {
    std::free(ptr);
    ESP_LOGD(tag, "Freed %p", ptr);
}

bool resource::do_is_equal(const memory_resource& other) const noexcept { return this == &other; }

} // namespace esp::memory
