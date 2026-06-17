#pragma once

#include <concepts>
#include <cstdint>
#include <type_traits>

#include "esp_tensor_concepts.hpp"

namespace esp::tensor {

template <typename T>
concept supported_integer =
    same_as_any<std::remove_cv_t<T>, std::int8_t, std::int16_t, std::int32_t>;

namespace detail {

template <class T> struct wider_integer {
    using type = std::conditional_t<(sizeof(T) < sizeof(std::int32_t)), std::int32_t, std::int64_t>;
};

template <class T> using wider_integer_t = typename wider_integer<T>::type;

template <supported_integer T>
[[nodiscard]] constexpr wider_integer_t<std::remove_cv_t<T>> widen(T value) {
    return static_cast<wider_integer_t<std::remove_cv_t<T>>>(value);
}

} // namespace detail

} // namespace esp::tensor
