#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <ranges>
#include <type_traits>

#include "esp_tensor_domain_concepts.hpp"
#include "esp_tensor_numeric.hpp"

namespace esp::tensor {

namespace quantization {

template <supported_integer output_t, std::size_t source_fractional_bits>
[[nodiscard]] constexpr output_t requantize_fixed_point(std::int64_t value)
    requires(source_fractional_bits <= std::numeric_limits<std::int64_t>::digits)
{
    constexpr auto shift =
        static_cast<std::ptrdiff_t>(source_fractional_bits) -
        static_cast<std::ptrdiff_t>(domain::fixed_point<output_t>::fractional_bits);

    if constexpr (shift > 0) {
        constexpr auto rounding_offset = std::int64_t{1} << (shift - 1);
        if (value > std::numeric_limits<std::int64_t>::max() - rounding_offset) {
            return std::numeric_limits<output_t>::max();
        }

        const auto rounded = (value + rounding_offset) >> shift;
        return std::saturating_cast<output_t>(rounded);
    } else if constexpr (shift < 0) {
        constexpr auto left_shift = -shift;
        constexpr auto output_min = std::numeric_limits<output_t>::lowest();
        constexpr auto output_max = std::numeric_limits<output_t>::max();
        constexpr auto lower = static_cast<std::int64_t>(output_min) >> left_shift;
        constexpr auto upper = static_cast<std::int64_t>(output_max) >> left_shift;

        if (value < lower) {
            return output_min;
        }
        if (value > upper) {
            return output_max;
        }
        return static_cast<output_t>(value << left_shift);
    } else {
        return std::saturating_cast<output_t>(value);
    }
}

} // namespace quantization

/**
 * @brief Converts fixed-point tensor values into an unquantized floating-point
 * tensor.
 *
 * This uses the source fixed-point domain fractional width to map raw integer
 * values into the destination floating-point domain.
 */
inline void requantize_impl(const detail::fixed_point_tensor_view auto input,
                            detail::no_quantized_tensor_view auto output) {
    using input_domain_t = typename std::remove_cvref_t<decltype(input)>::domain_type;
    using output_value_t = typename std::remove_cvref_t<decltype(output)>::domain_type::value_type;

    constexpr auto scale =
        output_value_t{1} /
        static_cast<output_value_t>(std::int64_t{1} << input_domain_t::fractional_bits);

    std::ranges::transform(input, output.begin(), [scale](const auto value) {
        return static_cast<output_value_t>(value) * scale;
    });
}

} // namespace esp::tensor
