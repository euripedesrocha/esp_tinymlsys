#pragma once
#include <type_traits>
namespace esp::tensor {
template <class Domain>
concept has_value_type = requires { typename std::remove_cvref_t<Domain>::value_type; };
template <class Shape>
concept defines_rank = requires { std::remove_cvref_t<Shape>::rank_value; };
} // namespace esp::tensor
