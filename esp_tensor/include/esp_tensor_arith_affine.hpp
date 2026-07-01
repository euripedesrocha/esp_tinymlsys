#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <cmath>
#include <limits>
#include <numeric>
#include <type_traits>

#include "esp_tensor_domain_concepts.hpp"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_numeric.hpp"

#include "esp_nn.h"

namespace esp::tensor::arith {

/**
 * @brief Plan to affine add parameters reusable across tensors with the same domains.
 */
struct affine_add_plan {
    std::int32_t lhs_multiplier = 0;
    std::int32_t rhs_multiplier = 0;
    int lhs_shift = 0;
    int rhs_shift = 0;
    int left_shift = 0;
    std::int32_t lhs_zero_point = 0;
    std::int32_t rhs_zero_point = 0;
    std::int32_t out_zero_point = 0;
    std::int32_t lhs_input_offset = 0;
    std::int32_t rhs_input_offset = 0;
    int lhs_input_shift = 0;
    int rhs_input_shift = 0;
    std::int32_t out_multiplier = std::numeric_limits<std::int32_t>::max();
    int out_shift = 0;
    std::int32_t activation_min = std::numeric_limits<std::int8_t>::lowest();
    std::int32_t activation_max = std::numeric_limits<std::int8_t>::max();
};

} // namespace esp::tensor::arith

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

/**
 * @brief Stores the integer multiplier and exponent used for affine re-quantization.
 */
struct quantized_multiplier {
    std::int32_t multiplier = 0;
    int shift = 0;
};

[[nodiscard]] constexpr std::int32_t input_offset(std::int32_t zero_point) { return -zero_point; }

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
                                                      const affine_domain auto& output_domain)
    pre(std::isfinite(static_cast<double>(input_domain.scale)) &&
        static_cast<double>(input_domain.scale) > 0)
        pre(std::isfinite(static_cast<double>(output_domain.scale)) &&
            static_cast<double>(output_domain.scale) > 0) {
    return make_quantized_multiplier(static_cast<double>(input_domain.scale) /
                                     static_cast<double>(output_domain.scale));
}

[[nodiscard]] inline arith::affine_add_plan make_affine_add_plan(const affine_domain auto& lhs,
                                                                 const affine_domain auto& rhs,
                                                                 const affine_domain auto& out) {
    const auto lhs_multiplier = scale_ratio(lhs, out);
    const auto rhs_multiplier = scale_ratio(rhs, out);
    const int left_shift = std::max({0, lhs_multiplier.shift, rhs_multiplier.shift});
    const auto lhs_zero_point = static_cast<std::int32_t>(lhs.zero_point);
    const auto rhs_zero_point = static_cast<std::int32_t>(rhs.zero_point);

    return {
        .lhs_multiplier = lhs_multiplier.multiplier,
        .rhs_multiplier = rhs_multiplier.multiplier,
        .lhs_shift = lhs_multiplier.shift,
        .rhs_shift = rhs_multiplier.shift,
        .left_shift = left_shift,
        .lhs_zero_point = lhs_zero_point,
        .rhs_zero_point = rhs_zero_point,
        .out_zero_point = static_cast<std::int32_t>(out.zero_point),
        .lhs_input_offset = input_offset(lhs_zero_point),
        .rhs_input_offset = input_offset(rhs_zero_point),
        .lhs_input_shift = lhs_multiplier.shift - left_shift,
        .rhs_input_shift = rhs_multiplier.shift - left_shift,
    };
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

        return std::saturating_cast<std::int32_t>(static_cast<std::int64_t>(value) *
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
        shifted = std::saturating_cast<std::int32_t>(static_cast<std::int64_t>(value) *
                                                     (std::int64_t{1} << left_shift));
    }
    return scale_by_power_of_two(sat_round_doubling_high_mul(shifted, multiplier.multiplier),
                                 left_shift - multiplier.shift);
}

/**
 * @brief Adds two affine-quantized spans in the prepared output domain.
 *
 * This rescales both inputs with integer re-quantization and stores the result
 * in the output affine domain.
 *
 */
inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out, const arith::affine_add_plan& plan) {
    std::ranges::transform(
        lhs, rhs, out.begin(), [&plan](const std::int8_t lhs_value, const std::int8_t rhs_value) {
            const auto lhs_term = requantize_shared_shift_add_input(
                centered_affine_value(lhs_value, plan.lhs_zero_point),
                {.multiplier = plan.lhs_multiplier, .shift = plan.lhs_shift}, plan.left_shift);
            const auto rhs_term = requantize_shared_shift_add_input(
                centered_affine_value(rhs_value, plan.rhs_zero_point),
                {.multiplier = plan.rhs_multiplier, .shift = plan.rhs_shift}, plan.left_shift);
            return std::saturating_cast<std::int8_t>(
                lhs_term + rhs_term + static_cast<std::int32_t>(plan.out_zero_point));
        });
}

inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out) {
    add(lhs, rhs, out, make_affine_add_plan(lhs.domain(), rhs.domain(), out.domain()));
}

namespace esp_nn {

[[nodiscard]] constexpr bool input_offset_fits_s8(std::int32_t input_offset) {
    return input_offset >= std::numeric_limits<std::int8_t>::lowest() &&
           input_offset <= std::numeric_limits<std::int8_t>::max();
}

[[nodiscard]] constexpr bool add_plan_is_supported(const arith::affine_add_plan& plan) {
    return plan.left_shift < 31 && input_offset_fits_s8(plan.lhs_input_offset) &&
           input_offset_fits_s8(plan.rhs_input_offset) && plan.lhs_input_shift <= 0 &&
           plan.rhs_input_shift <= 0 && plan.out_shift <= 0;
}

inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out, const arith::affine_add_plan& plan)
    pre(add_plan_is_supported(plan)) {
    ::esp_nn_add_elementwise_s8(
        lhs.data(), rhs.data(), plan.lhs_input_offset, plan.rhs_input_offset, plan.lhs_multiplier,
        plan.rhs_multiplier, plan.lhs_input_shift, plan.rhs_input_shift, plan.left_shift,
        out.data(), plan.out_zero_point, plan.out_multiplier, plan.out_shift, plan.activation_min,
        plan.activation_max, static_cast<std::int32_t>(out.size()));
}

inline void add(const affine_tensor_view auto lhs, const affine_tensor_view auto rhs,
                affine_tensor_view auto out) {
    add(lhs, rhs, out, make_affine_add_plan(lhs.domain(), rhs.domain(), out.domain()));
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
 * Contract preconditions require finite positive quantization scales. The
 * accelerated backend also asserts that the prepared quantization plan is
 * supported by ESP-NN.
 */
inline void add_impl(const detail::affine_tensor_view auto lhs,
                     const detail::affine_tensor_view auto rhs, detail::affine_tensor_view auto out,
                     const arith::affine_add_plan& plan) {
    if constexpr (hal::active_target() == hal::target::esp32s3 ||
                  hal::active_target() == hal::target::esp32p4) {
        detail::esp_nn::add(lhs, rhs, out, plan);
        return;
    }

    detail::add(lhs, rhs, out, plan);
}

inline void add_impl(const detail::affine_tensor_view auto lhs,
                     const detail::affine_tensor_view auto rhs,
                     detail::affine_tensor_view auto out) {
    add_impl(lhs, rhs, out, detail::make_affine_add_plan(lhs.domain(), rhs.domain(), out.domain()));
}

} // namespace esp::tensor

namespace esp::tensor::arith {

[[nodiscard]] inline affine_add_plan plan_add(const detail::affine_domain auto& lhs,
                                              const detail::affine_domain auto& rhs,
                                              const detail::affine_domain auto& out) {
    return detail::make_affine_add_plan(lhs, rhs, out);
}

inline void add(const detail::affine_tensor_view auto lhs,
                const detail::affine_tensor_view auto rhs, detail::affine_tensor_view auto out,
                const affine_add_plan& plan)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)> &&
             same_tensor_shape<decltype(lhs), decltype(rhs), decltype(out)> &&
             same_tensor_value_type<decltype(lhs), decltype(rhs), decltype(out)>
pre(lhs.shape() == rhs.shape()) pre(lhs.shape() == out.shape()) {
    add_impl(lhs, rhs, out, plan);
}

} // namespace esp::tensor::arith
