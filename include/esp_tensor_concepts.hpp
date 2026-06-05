#pragma once

#include <cstddef>
#include <concepts>
#include <ranges>
#include <type_traits>

namespace esp::tensor {

template <class Domain>
concept has_value_type = requires { typename std::remove_cvref_t<Domain>::value_type; };

template <class Shape>
concept defines_rank = requires { std::remove_cvref_t<Shape>::rank_value; };

template <class T>
concept tensor_signature =
    requires {
        typename std::remove_cvref_t<T>::domain_type;
        typename std::remove_cvref_t<T>::shape_type;
    } && has_value_type<typename std::remove_cvref_t<T>::domain_type> &&
    defines_rank<typename std::remove_cvref_t<T>::shape_type>;

template <class T>
concept tensor_type = tensor_signature<T> && requires(T& tensor) {
    {
        tensor.data()
    } -> std::convertible_to<typename std::remove_cvref_t<T>::domain_type::value_type*>;
    { tensor.shape() } -> std::convertible_to<typename std::remove_cvref_t<T>::shape_type>;
    { tensor.domain() } -> std::convertible_to<typename std::remove_cvref_t<T>::domain_type>;
    { tensor.size() } -> std::convertible_to<std::size_t>;
};

template <class TensorLike, class ValueType, class Shape, class Domain>
concept compatible_tensor_type = tensor_type<TensorLike> && requires(TensorLike& tensor) {
    { tensor.data() } -> std::convertible_to<ValueType*>;
    { tensor.shape() } -> std::convertible_to<Shape>;
    { tensor.domain() } -> std::convertible_to<Domain>;
};

template <class Range, class ValueType>
concept contiguous_tensor_source = std::ranges::contiguous_range<Range> &&
                                   std::ranges::sized_range<Range> && requires(Range& range) {
                                       {
                                           std::ranges::data(range)
                                       } -> std::convertible_to<ValueType*>;
                                   };

} // namespace esp::tensor
