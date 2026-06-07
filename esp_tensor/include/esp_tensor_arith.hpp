#pragma once

#include <algorithm>
#include <ranges>
#include <type_traits>

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_domain.hpp"

namespace esp::tensor::detail {

template <class Domain>
concept no_quantization_domain = specialization_of<Domain, domain::no_quantization>;

template <class View>
concept no_quantized_tensor_view =
    tensor_type<View> && std::ranges::input_range<View> &&
    no_quantization_domain<typename std::remove_cvref_t<View>::domain_type>;
} // namespace esp::tensor::detail

namespace esp::tensor {

/**
 * @brief Implements elementwise add for unquantized tensor views.
 *
 * This evaluates the reference elementwise loop using the value type's normal
 * `+` semantics.
 */
inline void add_impl(detail::no_quantized_tensor_view auto lhs,
                     detail::no_quantized_tensor_view auto rhs,
                     detail::no_quantized_tensor_view auto out) {
    using value_type = typename std::remove_cvref_t<decltype(lhs)>::domain_type::value_type;
    std::ranges::transform(lhs, rhs, out.begin(),
                           [](const value_type& lhs_value, const value_type& rhs_value) {
                               return static_cast<value_type>(lhs_value + rhs_value);
                           });
}

} // namespace esp::tensor
