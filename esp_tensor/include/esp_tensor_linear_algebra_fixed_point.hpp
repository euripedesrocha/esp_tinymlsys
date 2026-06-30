#pragma once

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <numeric>
#include <type_traits>

#include "dsps_dotprod.h"
#include "esp_tensor_domain_concepts.hpp"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_quantization_fixed_point.hpp"

namespace esp::tensor::detail {

inline std::int64_t scalar_dot(const fixed_point_tensor_view auto lhs,
                               const fixed_point_tensor_view auto rhs)
    requires same_tensor_domain<decltype(lhs), decltype(rhs)>
{
    return std::transform_reduce(lhs.begin(), lhs.end(), rhs.begin(), std::int64_t{}, std::plus<>{},
                                 [](const auto lhs_value, const auto rhs_value) {
                                     return static_cast<std::int64_t>(lhs_value) *
                                            static_cast<std::int64_t>(rhs_value);
                                 });
}

} // namespace esp::tensor::detail

extern "C" std::int64_t esp_tensor_dot_s16_raw_esp32s3(const std::int16_t* lhs,
                                                       const std::int16_t* rhs, int len);

namespace esp::tensor::detail::esp32s3 {

template <class T>
    requires same_as_any<T, std::int8_t, std::int16_t>
consteval std::size_t dot_batch_size_for() {
    // ESP32-S3 PIE uses 128-bit QR registers. Dot-product ACCX instructions
    // consume one full register: 16 signed 8-bit lanes or 8 signed 16-bit lanes.
    if constexpr (std::same_as<T, std::int8_t>) {
        return 16;
    } else {
        return 8;
    }
}

template <class T>
    requires same_as_any<T, std::int8_t, std::int16_t>
consteval std::int64_t dot_accumulator_max_for() {
    if constexpr (std::same_as<T, std::int8_t>) {
        // dsps_dp_s8 exposes an int32_t destination, so chunking must keep
        // each call inside that API contract even though ACCX is wider.
        return std::numeric_limits<std::int32_t>::max();
    } else {
        // EE.VMULAS.S16.ACCX accumulates into ESP32-S3 ACCX, a signed 40-bit
        // saturating register with positive limit 2^39 - 1.
        return (std::int64_t{1} << 39) - 1;
    }
}

template <class T>
    requires same_as_any<T, std::int8_t, std::int16_t>
consteval std::size_t max_dot_batch_elements_for() {
    constexpr auto batch_size = dot_batch_size_for<T>();
    constexpr auto max_abs_value = std::int64_t{1} << std::numeric_limits<T>::digits;
    constexpr auto max_product = max_abs_value * max_abs_value;
    auto max_elements = static_cast<std::size_t>(dot_accumulator_max_for<T>() / max_product);
    return max_elements / batch_size * batch_size;
}

inline std::int32_t raw_dot_s8(const std::int8_t* lhs, const std::int8_t* rhs, std::size_t size)
    pre(hal::is_aligned(lhs, hal::esp32s3::simd_alignment))
        pre(hal::is_aligned(rhs, hal::esp32s3::simd_alignment)) {

    auto result = std::int32_t{};
    dsps_dp_s8(lhs, rhs, &result, static_cast<int>(size));
    return result;
}

inline std::int64_t raw_dot_s16(const std::int16_t* lhs, const std::int16_t* rhs, std::size_t size)
    pre(hal::is_aligned(lhs, hal::esp32s3::simd_alignment))
        pre(hal::is_aligned(rhs, hal::esp32s3::simd_alignment)) {

    return esp_tensor_dot_s16_raw_esp32s3(lhs, rhs, static_cast<int>(size));
}

inline std::int64_t dot_impl(const fixed_point_tensor_view auto lhs,
                             const fixed_point_tensor_view auto rhs)
    requires same_tensor_domain<decltype(lhs), decltype(rhs)>
{
    using value_type = typename std::remove_cvref_t<decltype(lhs)>::value_type;

    constexpr auto batch_size = dot_batch_size_for<value_type>();
    constexpr auto max_chunk_size = max_dot_batch_elements_for<value_type>();
    static_assert(max_chunk_size >= batch_size);
    static_assert(max_chunk_size % batch_size == 0);
    const auto processed = lhs.size() / batch_size * batch_size;

    auto result = std::int64_t{};
    for (auto offset = std::size_t{}; offset < processed;) {
        const auto remaining = processed - offset;
        const auto chunk_size = remaining < max_chunk_size ? remaining : max_chunk_size;
        if constexpr (std::same_as<value_type, std::int8_t>) {
            result += raw_dot_s8(lhs.data() + offset, rhs.data() + offset, chunk_size);
        } else {
            result += raw_dot_s16(lhs.data() + offset, rhs.data() + offset, chunk_size);
        }
        offset += chunk_size;
    }

    if (processed != lhs.size()) {
        const auto tail = static_cast<std::ptrdiff_t>(processed);
        result += std::transform_reduce(
            lhs.begin() + tail, lhs.end(), rhs.begin() + tail, std::int64_t{}, std::plus<>{},
            [](const value_type lhs_value, const value_type rhs_value) {
                return static_cast<std::int64_t>(lhs_value) * static_cast<std::int64_t>(rhs_value);
            });
    }

    return result;
}

} // namespace esp::tensor::detail::esp32s3

namespace esp::tensor {

inline void dot_impl(const detail::fixed_point_tensor_view auto lhs,
                     const detail::fixed_point_tensor_view auto rhs,
                     detail::fixed_point_tensor_view auto out)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)>
pre(lhs.shape() == rhs.shape()) pre(out.size() == 1) {
    using value_type = typename std::remove_cvref_t<decltype(lhs)>::value_type;
    using out_value_type = typename std::remove_cvref_t<decltype(out)>::value_type;

    auto result = int64_t{};

    if constexpr ((hal::active_target() == hal::target::esp32s3) &&
                  same_as_any<value_type, std::int8_t, std::int16_t>) {
        result = detail::esp32s3::dot_impl(lhs, rhs);
    } else {
        result = detail::scalar_dot(lhs, rhs);
    }

    constexpr auto result_fractional_bits = 2 * domain::fixed_point<value_type>::fractional_bits;
    out[0] = quantization::requantize_fixed_point<out_value_type, result_fractional_bits>(result);
}

} // namespace esp::tensor
