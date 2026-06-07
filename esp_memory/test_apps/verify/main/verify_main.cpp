#include <memory_resource>
#include <vector>

#include "esp_memory_resource.hpp"

extern "C" void app_main(void) {
    using esp::memory::accelerator_capabilities;
    using esp::memory::accelerator_resource;
    using esp::memory::capabilities;

    accelerator_resource resource(capabilities::dma | capabilities::internal |
                                  accelerator_capabilities());
    std::pmr::vector<std::byte> buffer(&resource);
    buffer.resize(32);
}
