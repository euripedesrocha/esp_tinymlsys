#pragma once

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_arith.hpp"
#include "esp_tensor_linear_algebra.hpp"

namespace esp::tensor {

inline void add_impl(auto /*lhs*/, auto /*rhs*/, auto /*out*/) {
    static_assert(false, "tensor add is not implemented for these tensor operand types");
}

inline void dot_impl(auto /*unused*/, auto /*unused*/, auto /*unused*/) {
    static_assert(false, "tensor dot is not implemented for these tensor operand types");
}

inline void requantize_impl(auto /*input*/, auto /*output*/) {
    static_assert(false, "tensor requantize is not implemented for these tensor operand types");
}

} // namespace esp::tensor

namespace esp::tensor::arith {

inline void add(const tensor_view_like auto lhs, const tensor_view_like auto rhs,
                tensor_view_like auto out)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)> &&
             same_tensor_shape<decltype(lhs), decltype(rhs), decltype(out)> &&
             same_tensor_value_type<decltype(lhs), decltype(rhs), decltype(out)>
pre(lhs.shape() == rhs.shape()) pre(lhs.shape() == out.shape()) {
    add_impl(lhs, rhs, out);
}

} // namespace esp::tensor::arith

namespace esp::tensor::linear_algebra {

inline void dot(const tensor_view_like auto lhs, const tensor_view_like auto rhs,
                tensor_view_like auto out)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)>
pre(lhs.shape() == rhs.shape()) pre(out.size() == 1) {
    dot_impl(lhs, rhs, out);
}

} // namespace esp::tensor::linear_algebra

namespace esp::tensor::quantization {

inline void requantize(const tensor_view_like auto input, tensor_view_like auto output)
    requires same_tensor_shape<decltype(input), decltype(output)>
pre(input.shape() == output.shape()) {
    requantize_impl(input, output);
}

} // namespace esp::tensor::quantization
