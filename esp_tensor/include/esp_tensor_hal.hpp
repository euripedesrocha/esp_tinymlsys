#pragma once

#include <cstddef>
#include <cstdint>

namespace esp::tensor::hal {

enum class target : uint8_t {
    unknown,
    esp32s3,
    esp32p4,
};

[[nodiscard]] consteval target active_target() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    return target::esp32s3;
#elif defined(CONFIG_IDF_TARGET_ESP32P4)
    return target::esp32p4;
#else
    return target::unknown;
#endif
}

namespace esp32s3 {

inline constexpr std::size_t simd_alignment = 16;

} // namespace esp32s3

[[nodiscard]] bool is_aligned(const auto* pointer, const std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(pointer) % alignment) == 0;
}

template <class T> [[nodiscard]] consteval bool is_type_aligned(const std::size_t alignment) {
    return alignof(T) >= alignment;
}

} // namespace esp::tensor::hal
