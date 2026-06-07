#pragma once

#include <algorithm>
#include <concepts>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace esp::tensor {

template <typename T>
concept supported_integer = std::same_as<std::remove_cv_t<T>, std::int8_t> ||
                            std::same_as<std::remove_cv_t<T>, std::int16_t> ||
                            std::same_as<std::remove_cv_t<T>, std::int32_t>;

namespace detail {

template <class T> struct wider_integer {
    using type = std::conditional_t<(sizeof(T) < sizeof(std::int32_t)), std::int32_t, std::int64_t>;
};

template <class T> using wider_integer_t = typename wider_integer<T>::type;

template <typename To, typename From> [[nodiscard]] constexpr To saturate_cast(From value) {
    static_assert(std::numeric_limits<To>::is_integer);
    const auto lower = static_cast<From>(std::numeric_limits<To>::lowest());
    const auto upper = static_cast<From>(std::numeric_limits<To>::max());
    return static_cast<To>(std::clamp(value, lower, upper));
}

template <supported_integer T>
[[nodiscard]] constexpr wider_integer_t<std::remove_cv_t<T>> widen(T value) {
    return static_cast<wider_integer_t<std::remove_cv_t<T>>>(value);
}

} // namespace detail

} // namespace esp::tensor
