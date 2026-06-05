#pragma once

namespace esp::tensor::domain {

/**
 * @brief Marker type for tensors that do not use quantization metadata.
 *
 * This type identifies tensors whose stored values already represent the
 * intended numerical domain directly.
 *
 */
template <class T = float> struct no_quantization {
    /** @brief Scalar type stored by tensors in this domain. */
    using value_type = T;
};

} // namespace esp::tensor::domain
