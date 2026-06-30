#pragma once

#include <concepts>
#include <numeric>

#include "dsps_dotprod.h"
#include "esp_tensor_hal.hpp"
#include "esp_tensor_domain_concepts.hpp"

namespace esp::tensor {

inline void dot_impl(const detail::no_quantized_tensor_view auto lhs,
                     const detail::no_quantized_tensor_view auto rhs,
                     detail::no_quantized_tensor_view auto out)
    requires same_tensor_domain<decltype(lhs), decltype(rhs), decltype(out)>
pre(lhs.shape() == rhs.shape()) pre(out.size() == 1) {
    using input_value_type = typename decltype(lhs)::domain_type::value_type;
    using output_value_type = typename decltype(out)::domain_type::value_type;

    if constexpr (std::same_as<input_value_type, float> && std::same_as<output_value_type, float>) {
        if constexpr (hal::active_target() == hal::target::esp32s3 ||
                      hal::active_target() == hal::target::esp32p4) {
            dsps_dotprod_f32(lhs.data(), rhs.data(), out.data(), static_cast<int>(lhs.size()));
            return;
        }
    }

    out.data()[0] = std::transform_reduce(lhs.begin(), lhs.end(), rhs.begin(), input_value_type{});
}

} // namespace esp::tensor
