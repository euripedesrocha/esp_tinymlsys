#pragma once

#include <concepts>
#include <ranges>
#include <type_traits>

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_domain.hpp"

namespace esp::tensor::detail {

template <class Domain>
concept no_quantization_domain = specialization_of<Domain, domain::no_quantization>;

template <class Domain>
concept fixed_point_domain = specialization_of<Domain, domain::fixed_point>;

template <class Domain>
concept affine_domain = specialization_of<Domain, domain::affine>;

template <class View>
concept no_quantized_tensor_view =
    tensor_view_like<View> && std::ranges::input_range<View> &&
    no_quantization_domain<typename std::remove_cvref_t<View>::domain_type>;

template <class View>
concept fixed_point_tensor_view =
    tensor_view_like<View> && std::ranges::input_range<View> &&
    fixed_point_domain<typename std::remove_cvref_t<View>::domain_type>;

template <class View>
concept affine_tensor_view = tensor_view_like<View> && std::ranges::input_range<View> &&
                             affine_domain<typename std::remove_cvref_t<View>::domain_type>;

} // namespace esp::tensor::detail
