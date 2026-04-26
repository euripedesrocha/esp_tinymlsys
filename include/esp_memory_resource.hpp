#pragma once

#include <cstddef>
#include <cstdint>
#include <memory_resource>
#include <type_traits>

#include "esp_heap_caps.h"
#include "sdkconfig.h"

namespace esp::memory {

namespace detail {

#if defined(MALLOC_CAP_EXEC)
inline constexpr uint32_t executable_cap = MALLOC_CAP_EXEC;
#else
inline constexpr uint32_t executable_cap = 0;
#endif

#if defined(MALLOC_CAP_SIMD)
inline constexpr uint32_t simd_cap = MALLOC_CAP_SIMD;
#else
inline constexpr uint32_t simd_cap = 0;
#endif

constexpr std::size_t natural_alignment = alignof(std::max_align_t);

enum class target : std::uint8_t {
    unknown,
    esp32,
    esp32c2,
    esp32c3,
    esp32c5,
    esp32c6,
    esp32c61,
    esp32h2,
    esp32h21,
    esp32h4,
    esp32p4,
    esp32s2,
    esp32s3,
    esp32s31,
};

enum class alignment_profile : std::uint8_t {
    accelerator,
};

inline constexpr target current_target =
#if defined(CONFIG_IDF_TARGET_ESP32)
    target::esp32;
#elif defined(CONFIG_IDF_TARGET_ESP32C2)
    target::esp32c2;
#elif defined(CONFIG_IDF_TARGET_ESP32C3)
    target::esp32c3;
#elif defined(CONFIG_IDF_TARGET_ESP32C5)
    target::esp32c5;
#elif defined(CONFIG_IDF_TARGET_ESP32C6)
    target::esp32c6;
#elif defined(CONFIG_IDF_TARGET_ESP32C61)
    target::esp32c61;
#elif defined(CONFIG_IDF_TARGET_ESP32H2)
    target::esp32h2;
#elif defined(CONFIG_IDF_TARGET_ESP32H21)
    target::esp32h21;
#elif defined(CONFIG_IDF_TARGET_ESP32H4)
    target::esp32h4;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    target::esp32p4;
#elif defined(CONFIG_IDF_TARGET_ESP32S2)
    target::esp32s2;
#elif defined(CONFIG_IDF_TARGET_ESP32S3)
    target::esp32s3;
#elif defined(CONFIG_IDF_TARGET_ESP32S31)
    target::esp32s31;
#else
    target::unknown;
#endif

template <alignment_profile Profile, target Target = current_target>
consteval std::size_t alignment_for() {
    if constexpr (Profile == alignment_profile::accelerator) {
        if constexpr (Target == target::esp32s3 || Target == target::esp32p4) {
            return 16;
        }
    }

    return natural_alignment;
}

// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
constexpr std::size_t effective_alignment(std::size_t requested, std::size_t minimum) {
    const auto normalized = requested == 0 ? natural_alignment : requested;
    return normalized < minimum ? minimum : normalized;
}

} // namespace detail

enum class capabilities : uint32_t {
    not_specified = MALLOC_CAP_DEFAULT,
    addressable_8bit = MALLOC_CAP_8BIT,
    addressable_32bit = MALLOC_CAP_32BIT,
    dma = MALLOC_CAP_DMA,
    executable = detail::executable_cap,
    from_spiram = MALLOC_CAP_SPIRAM,
    internal = MALLOC_CAP_INTERNAL,
    retention_dma = MALLOC_CAP_RETENTION,
    rtc = MALLOC_CAP_RTCRAM,
    simd = detail::simd_cap,
};

constexpr capabilities operator|(capabilities lhs, capabilities rhs) {
    return static_cast<capabilities>(static_cast<std::underlying_type_t<capabilities>>(lhs) |
                                     static_cast<std::underlying_type_t<capabilities>>(rhs));
}

constexpr uint32_t get_caps(capabilities cap) {
    return static_cast<std::underlying_type_t<capabilities>>(cap);
}

constexpr std::size_t accelerator_alignment() {
    return detail::alignment_for<detail::alignment_profile::accelerator>();
}

constexpr capabilities accelerator_capabilities() { return capabilities::simd; }

class resource : public std::pmr::memory_resource {
public:
    static constexpr const char* tag = "esp_memory_resource";

    explicit resource(
        capabilities requested_caps = capabilities::not_specified,
        std::pmr::memory_resource* upstream_resource = std::pmr::get_default_resource())
        : resource(requested_caps, upstream_resource, 1) {}

    [[nodiscard]] capabilities caps() const noexcept { return caps_; }
    [[nodiscard]] std::size_t minimum_alignment() const noexcept { return min_alignment_; }

protected:
    resource(capabilities requested_caps, std::pmr::memory_resource* upstream_resource,
             std::size_t minimum_alignment)
        : caps_(requested_caps), upstream_(upstream_resource), min_alignment_(minimum_alignment) {}

private:
    void* do_allocate(size_t bytes, size_t alignment) override;
    void do_deallocate(void* ptr, size_t bytes, size_t alignment) override;
    [[nodiscard]] bool do_is_equal(const memory_resource& other) const noexcept override;

    capabilities caps_;
    std::pmr::memory_resource* upstream_;
    std::size_t min_alignment_;
};

template <detail::alignment_profile Profile> class aligned_resource : public resource {
public:
    static constexpr detail::alignment_profile profile = Profile;
    static constexpr std::size_t alignment = detail::alignment_for<Profile>();

    explicit aligned_resource(
        capabilities requested_caps = capabilities::not_specified,
        std::pmr::memory_resource* upstream_resource = std::pmr::get_default_resource())
        : resource(requested_caps, upstream_resource, alignment) {}
};

using accelerator_resource = aligned_resource<detail::alignment_profile::accelerator>;

} // namespace esp::memory
