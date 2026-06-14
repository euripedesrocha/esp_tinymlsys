#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <ranges>
#include <type_traits>

#include "dsps_add.h"
#include "esp_tensor_domain_concepts.hpp"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_invariant.hpp"
#include "esp_tensor_numeric.hpp"

namespace esp::tensor::detail {

namespace esp32s3 {

template <class T>
concept accelerated_fixed_point = std::same_as<T, std::int8_t> || std::same_as<T, std::int16_t>;

inline void add(const fixed_point_tensor_view auto lhs, const fixed_point_tensor_view auto rhs,
                fixed_point_tensor_view auto out) {
    using value_type = typename std::remove_cvref_t<decltype(out)>::domain_type::value_type;

    std::size_t processed = 0;

    if constexpr (accelerated_fixed_point<value_type>) {
        detail::validate_invariant(hal::is_aligned(lhs.data(), hal::esp32s3::simd_alignment),
                                   "esp32s3 fixed-point add requires aligned lhs storage");
        detail::validate_invariant(hal::is_aligned(rhs.data(), hal::esp32s3::simd_alignment),
                                   "esp32s3 fixed-point add requires aligned rhs storage");
        detail::validate_invariant(hal::is_aligned(out.data(), hal::esp32s3::simd_alignment),
                                   "esp32s3 fixed-point add requires aligned output storage");

        constexpr std::size_t simd_width = 16 / sizeof(value_type);
        processed = static_cast<std::size_t>(out.size() / simd_width * simd_width);

        if constexpr (std::same_as<value_type, std::int16_t>) {
            if (processed > 0) {
                dsps_add_s16_aes3(lhs.data(), rhs.data(), out.data(), static_cast<int>(processed),
                                  1, 1, 1, 0);
            }
        } else if constexpr (std::same_as<value_type, std::int8_t>) {
            if (processed > 0) {
                dsps_add_s8_aes3(lhs.data(), rhs.data(), out.data(), static_cast<int>(processed), 1,
                                 1, 1, 0);
            }
        }
    }

    std::transform(lhs.begin() + static_cast<std::ptrdiff_t>(processed), lhs.end(),
                   rhs.begin() + static_cast<std::ptrdiff_t>(processed),
                   out.begin() + static_cast<std::ptrdiff_t>(processed),
                   [](const value_type lhs_value, const value_type rhs_value) {
                       return detail::saturate_cast<value_type>(detail::widen(lhs_value) +
                                                                detail::widen(rhs_value));
                   });
}

} // namespace esp32s3

} // namespace esp::tensor::detail

namespace esp::tensor {

/**
 * @brief Implements elementwise add for fixed-point tensor views.
 *
 * This widens both inputs, adds them, and saturating-narrows the result back
 * to the fixed-point storage type after the facade has validated shared
 * preconditions.
 *
 */
inline void add_impl(const detail::fixed_point_tensor_view auto lhs,
                     const detail::fixed_point_tensor_view auto rhs,
                     detail::fixed_point_tensor_view auto out) {
    using value_type = typename std::remove_cvref_t<decltype(out)>::domain_type::value_type;

    if constexpr (hal::active_target() == hal::target::esp32s3) {
        detail::esp32s3::add(lhs, rhs, out);
    } else {
        std::ranges::transform(lhs, rhs, out.begin(),
                               [](const value_type lhs_value, const value_type rhs_value) {
                                   return detail::saturate_cast<value_type>(
                                       detail::widen(lhs_value) + detail::widen(rhs_value));
                               });
    }
}

} // namespace esp::tensor
