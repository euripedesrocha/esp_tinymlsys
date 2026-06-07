#include <cinttypes>
#include <cstdint>
#include <memory_resource>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_memory_resource.hpp"

constexpr auto TAG = "memory_resource_example";

namespace {

void log_heap_summary(const char* label, uint32_t caps) {
    multi_heap_info_t info{};
    heap_caps_get_info(&info, caps);

    ESP_LOGI(TAG, "%s: free=%zu largest=%zu allocated=%zu min_free=%zu blocks=%zu/%zu", label,
             info.total_free_bytes, info.largest_free_block, info.total_allocated_bytes,
             info.minimum_free_bytes, info.allocated_blocks, info.total_blocks);
}

} // namespace

extern "C" void app_main(void) {
    ESP_LOGI(TAG, "[APP] Startup..");
    ESP_LOGI(TAG, "[APP] Free memory: %" PRIu32 " bytes", esp_get_free_heap_size());
    ESP_LOGI(TAG, "[APP] IDF version: %s", esp_get_idf_version());

    auto memory_source = esp::memory::capabilities::dma | esp::memory::capabilities::internal |
                         esp::memory::accelerator_capabilities();
    const auto caps = esp::memory::get_caps(memory_source);

    ESP_LOGI(TAG, "Requested caps: 0x%08" PRIx32, caps);
    ESP_LOGI(TAG, "Accelerator alignment floor: %zu", esp::memory::accelerator_alignment());

    log_heap_summary("Before reserve", caps);

    esp::memory::accelerator_resource res{memory_source};
    std::pmr::vector<std::byte> data{&res};
    data.reserve(2048);

    const auto address = reinterpret_cast<std::uintptr_t>(data.data());
    const auto alignment = esp::memory::accelerator_alignment();
    const auto aligned = address % alignment == 0;

    ESP_LOGI(TAG, "Reserved %zu bytes, capacity=%zu, data=%p", std::size_t{2048}, data.capacity(),
             static_cast<void*>(data.data()));
    ESP_LOGI(TAG, "Pointer alignment check: %zu-byte aligned = %s", alignment,
             aligned ? "yes" : "no");

    log_heap_summary("After reserve", caps);
}
