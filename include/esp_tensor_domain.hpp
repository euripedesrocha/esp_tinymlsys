#pragma once

#include <concepts>
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

} // namespace esp::tensor::domain
