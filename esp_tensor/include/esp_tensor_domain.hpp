#pragma once

#include <concepts>
#include <cstdint>
#include <limits>

#include "esp_tensor_numeric.hpp"

namespace esp::tensor::domain {

template <class T>
concept arithmetic_scalar = std::signed_integral<T> || std::floating_point<T>;

/**
 * @brief Marker type for tensors that do not use quantization metadata.
 *
 * This type identifies tensors whose stored values already represent the
 * intended numerical domain directly.
 *
 */
template <arithmetic_scalar T = float> struct no_quantization {
    /** @brief Scalar type stored by tensors in this domain. */
    using value_type = T;
};

/**
 * @brief Describes the canonical saturating fixed-point domain for a signed
 * integer storage type.
 *
 * Arithmetic kernels for this domain are expected to follow saturating
 * fixed-point semantics when narrowing results back to the storage type. The
 * fractional width is derived from the storage type: Q7 for int8_t, Q15 for
 * int16_t, and Q31 for int32_t.
 *
 */
template <supported_integer T> struct fixed_point {
    /** @brief Integer storage type used by this fixed-point domain. */
    using value_type = T;
    /** @brief Number of fractional bits carried by values in this domain. */
    static constexpr std::size_t fractional_bits = std::numeric_limits<T>::digits;
};

/**
 * @brief Describes an affine quantization domain using scale and zero point.
 *
 * This metadata records how int8 tensor values map to real values through the
 * standard affine quantization formula.
 *
 */
template <class Scale = float, class ZeroPoint = std::int32_t> struct affine {
    /** @brief Integer storage type used by this quantized domain. */
    using value_type = std::int8_t;
    /** @brief Type used to store the affine scale parameter. */
    using scale_type = Scale;
    /** @brief Type used to store the affine zero-point parameter. */
    using zero_point_type = ZeroPoint;

    /** @brief Multiplicative scale that maps stored integers into real values. */
    scale_type scale = 1;
    /** @brief Integer offset that represents real zero in the stored domain. */
    zero_point_type zero_point = 0;
};

} // namespace esp::tensor::domain
