#pragma once

#include "esp_tensor_invariant.hpp"
#include "esp_tensor_concepts.hpp"
#include "esp_tensor_arith.hpp"

namespace esp::tensor {

inline void add_impl(auto, auto, auto) {
    static_assert(false, "tensor add is not implemented for these tensor operand types");
}

inline void add(tensor_type auto lhs, tensor_type auto rhs, tensor_type auto out)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)> &&
             same_tensor_shape<decltype(lhs), decltype(rhs), decltype(out)>
{
    detail::validate_invariant(lhs.shape() == rhs.shape(),
                               "tensor add requires matching input shapes");
    detail::validate_invariant(lhs.shape() == out.shape(),
                               "tensor add requires output shape to match inputs");
    add_impl(lhs, rhs, out);
}

} // namespace esp::tensor
