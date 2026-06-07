#pragma once

#include <array>
#include <concepts>
#include <cstddef>
#include <functional>
#include <numeric>
#include <utility>

#include "esp_tensor_mdspan_compat.hpp"

namespace esp::tensor {

template <std::size_t Rank> using dynamic_extents = detail::md::dextents<std::size_t, Rank>;

/**
 * @brief Stores the logical extents of a tensor with a fixed rank.
 *
 * This class represents the size of each tensor axis and provides a small,
 * constexpr-friendly container for shape information.
 *
 */
template <std::size_t Rank> class tensor_shape {
public:
    using size_type = std::size_t;
    static constexpr std::size_t rank_value = Rank;

    /**
     * @brief Creates a shape with all extents initialized to zero.
     *
     */
    constexpr tensor_shape() = default;

    /**
     * @brief Creates a shape from an array of extents.
     *
     */
    constexpr explicit tensor_shape(const std::array<size_type, Rank>& extents) : values(extents) {}

    template <class... Sizes>
        requires(sizeof...(Sizes) == Rank && (std::convertible_to<Sizes, size_type> && ...))
    /**
     * @brief Creates a shape from one extent argument per axis.
     *
     * This lets callers construct a shape directly from a rank-matching list of
     * sizes.
     *
     */
    constexpr explicit tensor_shape(Sizes... sizes) : values{static_cast<size_type>(sizes)...} {}

    /**
     * @brief Returns the extent of one axis.
     *
     */
    [[nodiscard]] constexpr size_type operator[](size_type axis) const { return values[axis]; }

    /**
     * @brief Returns an iterator to the first extent.
     *
     */
    [[nodiscard]] constexpr auto begin() const { return values.begin(); }
    /**
     * @brief Returns an iterator one past the last extent.
     *
     */
    [[nodiscard]] constexpr auto end() const { return values.end(); }

    /**
     * @brief Compares two shapes for exact axis-by-axis equality.
     *
     * This checks whether both shapes describe the same logical tensor extents.
     *
     */
    [[nodiscard]] friend constexpr bool operator==(const tensor_shape&,
                                                   const tensor_shape&) = default;

private:
    std::array<size_type, Rank> values{};
};

template <std::size_t Rank> using tensor_strides = tensor_shape<Rank>;

template <class Size, std::size_t Rank>
tensor_shape(const std::array<Size, Rank>&) -> tensor_shape<Rank>;

template <class... Sizes>
    requires(sizeof...(Sizes) > 0 && (std::convertible_to<Sizes, std::size_t> && ...))
tensor_shape(Sizes...) -> tensor_shape<sizeof...(Sizes)>;

namespace detail {

template <class Shape, std::size_t... Indices>
/**
 * @brief Converts a shape into an `mdspan` extents object.
 *
 * This expands the shape's axis sizes into the `mdspan` representation used by
 * tensor views.
 *
 * It is necessary because `tensor_shape` is the educational front-end type,
 * while `mdspan` needs its own extents object for layout-aware access.
 */
[[nodiscard]] constexpr dynamic_extents<Shape::rank_value>
make_extents_impl(const Shape& shape, std::index_sequence<Indices...> /*unused*/) {
    return dynamic_extents<Shape::rank_value>{shape[Indices]...};
}

template <class Shape>
/**
 * @brief Builds an `mdspan` extents object from a shape.
 *
 * This forwards the shape through an index-sequence based conversion helper.
 *
 * It is necessary to keep the public tensor code readable while still
 * producing the exact extents type expected by `mdspan`.
 */
[[nodiscard]] constexpr dynamic_extents<Shape::rank_value> make_extents(const Shape& shape) {
    return make_extents_impl(shape, std::make_index_sequence<Shape::rank_value>{});
}

template <class Shape>
/**
 * @brief Computes the logical number of elements described by a shape.
 *
 * This multiplies all extents together to obtain the total element count.
 *
 * It is necessary because storage allocation, span validation, and empty checks
 * all depend on the tensor's logical size rather than its rank alone.
 */
[[nodiscard]] constexpr std::size_t logical_element_count(const Shape& shape) {
    return std::accumulate(shape.begin(), shape.end(), std::size_t{1}, std::multiplies<>{});
}

} // namespace detail

} // namespace esp::tensor
