#pragma once

#include <algorithm>
#include <ranges>
#include <type_traits>

#include "esp_tensor_domain_concepts.hpp"

namespace esp::tensor {

/**
 * @brief Implements elementwise add for unquantized tensor views.
 *
 * This evaluates the reference elementwise loop using the value type's normal
 * `+` semantics.
 */
inline void add_impl(const detail::no_quantized_tensor_view auto lhs,
                     const detail::no_quantized_tensor_view auto rhs,
                     detail::no_quantized_tensor_view auto out) {
    using value_type = typename std::remove_cvref_t<decltype(lhs)>::domain_type::value_type;
    std::ranges::transform(lhs, rhs, out.begin(),
                           [](const value_type& lhs_value, const value_type& rhs_value) {
                               return static_cast<value_type>(lhs_value + rhs_value);
                           });
}

} // namespace esp::tensor
