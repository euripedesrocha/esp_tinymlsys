#pragma once

#include <algorithm>
#include <cstdint>
#include <ranges>
#include <type_traits>

#include "esp_tensor_domain_concepts.hpp"

namespace esp::tensor {

/**
 * @brief Converts fixed-point tensor values into an unquantized floating-point
 * tensor.
 *
 * This uses the source fixed-point domain fractional width to map raw integer
 * values into the destination floating-point domain.
 */
inline void requantize_impl(const detail::fixed_point_tensor_view auto input,
                            detail::floating_point_tensor_view auto output) {
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
