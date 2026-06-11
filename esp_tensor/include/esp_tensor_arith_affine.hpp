#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cmath>
#include <limits>
#include <type_traits>

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_domain.hpp"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_invariant.hpp"
#include "esp_tensor_numeric.hpp"

#include "esp_nn.h"

namespace esp::tensor::detail {

/**
 * @brief Removes the affine zero point from a quantized value.
 *
 * This converts a stored integer into the int32 accumulator domain used by
 * affine quantized arithmetic.
 *
 */
[[nodiscard]] constexpr std::int32_t centered_affine_value(supported_integer auto value,
                                                           std::int32_t zero_point) {
    return static_cast<std::int32_t>(value) - zero_point;
}

template <class Domain>
concept affine_domain = specialization_of<Domain, domain::affine>;

inline void validate_affine_scales(const affine_domain auto& input_domain,
                                   const affine_domain auto& output_domain) {
    esp::tensor::detail::validate_invariant(
        std::isfinite(static_cast<double>(input_domain.scale)) &&
            static_cast<double>(input_domain.scale) > 0,
        "affine input scale must be finite and positive");
    esp::tensor::detail::validate_invariant(
        std::isfinite(static_cast<double>(output_domain.scale)) &&
            static_cast<double>(output_domain.scale) > 0,
        "affine output scale must be finite and positive");
}

/**
 * @brief Stores the integer multiplier and exponent used for affine re-quantization.
 */
struct quantized_multiplier {
    std::int32_t multiplier = 0;
    int shift = 0;
};

[[nodiscard]] inline quantized_multiplier make_quantized_multiplier(double scale) {
    if (scale == 0.0 || !std::isfinite(scale)) {
        return {};
    }

    int exponent = 0;
    double significand = std::frexp(scale, &exponent);
    auto multiplier = std::llround(significand * static_cast<double>(std::int64_t{1} << 31));

    if (multiplier == (std::int64_t{1} << 31)) {
        multiplier /= 2;
        ++exponent;
    }

    return {
        .multiplier = static_cast<std::int32_t>(multiplier),
        .shift = exponent,
    };
}

[[nodiscard]] inline quantized_multiplier scale_ratio(const affine_domain auto& input_domain,
                                                      const affine_domain auto& output_domain) {
    validate_affine_scales(input_domain, output_domain);
    return make_quantized_multiplier(static_cast<double>(input_domain.scale) /
                                     static_cast<double>(output_domain.scale));
}

/**
 * @brief Scales by a signed power-of-two exponent.
 *
 * Positive exponents divide with rounding. Negative exponents multiply with
 * saturation.
 */
[[nodiscard]] constexpr std::int32_t scale_by_power_of_two(std::int32_t value, int exponent) {
    if (exponent < 0) {
        const int left_shift = -exponent;
        if (left_shift >= 31) {
            if (value > 0) {
                return std::numeric_limits<std::int32_t>::max();
            }
            if (value < 0) {
                return std::numeric_limits<std::int32_t>::min();
            }
            return 0;
        }

        return saturate_cast<std::int32_t>(static_cast<std::int64_t>(value) *
                                           (std::int64_t{1} << left_shift));
    }

    if (exponent == 0) {
        return value;
    }

    if (exponent >= 31) {
        return value >= 0 ? 0 : -1;
    }

    const auto mask = (static_cast<std::uint32_t>(1) << exponent) - 1U;
    const auto remainder = static_cast<std::uint32_t>(value) & mask;
    std::int32_t result = value >> exponent;
    const auto threshold = (mask >> 1) + static_cast<std::uint32_t>(result < 0);

    if (remainder > threshold) {
        result += 1;
    }

    return result;
}

[[nodiscard]] constexpr std::int32_t sat_round_doubling_high_mul(std::int32_t lhs,
                                                                 std::int32_t rhs) {
    const bool overflow = lhs == std::numeric_limits<std::int32_t>::min() &&
                          rhs == std::numeric_limits<std::int32_t>::min();
    std::int64_t nudge = static_cast<std::int64_t>(1) << 30;
    if ((lhs < 0) != (rhs < 0)) {
        nudge = 1 - nudge;
    }

    const std::int64_t product = (static_cast<std::int64_t>(lhs) * rhs) + nudge;
    const std::int32_t sign = static_cast<std::int32_t>(product >> 63);
    const std::int32_t to_add =
        sign & (static_cast<std::int32_t>((static_cast<std::uint32_t>(1) << 31) - 1));
    const auto result = static_cast<std::int32_t>((product + to_add) >> 31);
    return overflow ? std::numeric_limits<std::int32_t>::max() : result;
}

[[nodiscard]] constexpr std::int32_t
requantize_shared_shift_add_input(std::int32_t value, quantized_multiplier multiplier,
                                  int left_shift) {
    auto shifted = std::int32_t{};
    if (left_shift >= 31) {
        shifted = value > 0   ? std::numeric_limits<std::int32_t>::max()
                  : value < 0 ? std::numeric_limits<std::int32_t>::min()
                              : 0;
    } else {
        shifted = saturate_cast<std::int32_t>(static_cast<std::int64_t>(value) *
                                              (std::int64_t{1} << left_shift));
    }
    return scale_by_power_of_two(sat_round_doubling_high_mul(shifted, multiplier.multiplier),
                                 left_shift - multiplier.shift);
}

template <class View>
concept affine_tensor_view =
    tensor_view_like<View> && affine_domain<typename std::remove_cvref_t<View>::domain_type>;

/**
 * @brief Adds two affine-quantized spans in the prepared output domain.
 *
 * This rescales both inputs with integer re-quantization and stores the result
 * in the output affine domain.
 *
 */
inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out) {
    const auto& lhs_domain = lhs.domain();
    const auto& rhs_domain = rhs.domain();
    const auto& out_domain = out.domain();
    const auto lhs_multiplier = scale_ratio(lhs_domain, out_domain);
    const auto rhs_multiplier = scale_ratio(rhs_domain, out_domain);
    const int left_shift = std::max({0, lhs_multiplier.shift, rhs_multiplier.shift});

    std::ranges::transform(
        lhs, rhs, out.begin(),
        [&lhs_domain, &rhs_domain, &out_domain, lhs_multiplier, rhs_multiplier,
         left_shift](const std::int8_t lhs_value, const std::int8_t rhs_value) {
            const auto lhs_term = requantize_shared_shift_add_input(
                centered_affine_value(lhs_value, static_cast<std::int32_t>(lhs_domain.zero_point)),
                lhs_multiplier, left_shift);
            const auto rhs_term = requantize_shared_shift_add_input(
                centered_affine_value(rhs_value, static_cast<std::int32_t>(rhs_domain.zero_point)),
                rhs_multiplier, left_shift);
            return saturate_cast<std::int8_t>(lhs_term + rhs_term +
                                              static_cast<std::int32_t>(out_domain.zero_point));
        });
}

namespace esp_nn {

struct add_plan {
    quantized_multiplier lhs_multiplier;
    quantized_multiplier rhs_multiplier;
    std::int32_t lhs_zero_point = 0;
    std::int32_t rhs_zero_point = 0;
    std::int32_t out_zero_point = 0;
};

[[nodiscard]] inline add_plan plan_add(const auto& lhs, const auto& rhs, const auto& out)
    requires requires {
        lhs.scale;
        lhs.zero_point;
        rhs.scale;
        rhs.zero_point;
        out.scale;
        out.zero_point;
    }
{
    return {
        .lhs_multiplier = scale_ratio(lhs, out),
        .rhs_multiplier = scale_ratio(rhs, out),
        .lhs_zero_point = static_cast<std::int32_t>(lhs.zero_point),
        .rhs_zero_point = static_cast<std::int32_t>(rhs.zero_point),
        .out_zero_point = static_cast<std::int32_t>(out.zero_point),
    };
}

[[nodiscard]] constexpr int add_left_shift(const add_plan& plan) {
    return std::max({0, plan.lhs_multiplier.shift, plan.rhs_multiplier.shift});
}

[[nodiscard]] constexpr bool zero_point_offset_fits_s8(std::int32_t zero_point) {
    constexpr std::int32_t lowest_offset = -128;
    constexpr std::int32_t max_offset = 127;
    return zero_point >= -max_offset && zero_point <= -lowest_offset;
}

[[nodiscard]] constexpr std::int32_t input_offset(std::int32_t zero_point) { return -zero_point; }

[[nodiscard]] constexpr bool add_plan_is_supported(const add_plan& plan) {
    const int shift = add_left_shift(plan);
    return shift < 31 && zero_point_offset_fits_s8(plan.lhs_zero_point) &&
           zero_point_offset_fits_s8(plan.rhs_zero_point) &&
           plan.lhs_multiplier.shift - shift <= 0 && plan.rhs_multiplier.shift - shift <= 0;
}

inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out) {
    const auto plan = plan_add(lhs.domain(), rhs.domain(), out.domain());
    validate_invariant(add_plan_is_supported(plan),
                       "esp-nn affine add requires a supported quantization plan");
    const int shift = add_left_shift(plan);
    ::esp_nn_add_elementwise_s8(
        lhs.data(), rhs.data(), input_offset(plan.lhs_zero_point),
        input_offset(plan.rhs_zero_point), plan.lhs_multiplier.multiplier,
        plan.rhs_multiplier.multiplier, plan.lhs_multiplier.shift - shift,
        plan.rhs_multiplier.shift - shift, shift, out.data(), plan.out_zero_point,
        std::numeric_limits<std::int32_t>::max(), 0, std::numeric_limits<std::int8_t>::lowest(),
        std::numeric_limits<std::int8_t>::max(), static_cast<std::int32_t>(out.size()));
}

} // namespace esp_nn

} // namespace esp::tensor::detail

namespace esp::tensor {

/**
 * @brief Implements element wise add for affine-quantized tensor views.
 *
 * This prepares quantization parameters from the domains, flattens the views,
 * and executes the quantized add kernel after the facade has validated shared
 * preconditions.
 *
 * @throws std::invalid_argument When quantization scales are not finite or
 * positive, or when an accelerated affine plan is not supported by the backend.
 */
inline void add_impl(const detail::affine_tensor_view auto lhs,
                     const detail::affine_tensor_view auto rhs,
                     detail::affine_tensor_view auto out) {
    if constexpr (hal::active_target() == hal::target::esp32s3 ||
                  hal::active_target() == hal::target::esp32p4) {
        detail::esp_nn::add(lhs, rhs, out);
        return;
    }

    detail::add(lhs, rhs, out);
}

} // namespace esp::tensor
