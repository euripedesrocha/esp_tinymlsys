#pragma once

#include <concepts>
#include <cstddef>
#include <ranges>
#include <type_traits>
#include <utility>

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_domain.hpp"
#include "esp_tensor_shape.hpp"

namespace esp::tensor {

/**
 * @brief Provides a non-owning multidimensional view over contiguous tensor
 * data.
 *
 */
template <class T, has_value_type Domain = domain::no_quantization<std::remove_cv_t<T>>,
          defines_rank Shape = tensor_shape<1>>
    requires std::same_as<std::remove_cv_t<T>, typename Domain::value_type>
class tensor_view {
public:
    using domain_type = Domain;
    using shape_type = Shape;
    using value_type = typename domain_type::value_type;
    using element_type = T;
    using mdspan_type = detail::md::mdspan<element_type, dynamic_extents<shape_type::rank_value>>;
    using size_type = std::size_t;

    static constexpr std::size_t rank_value = shape_type::rank_value;

    /**
     * @brief Creates an empty default-initialized tensor view.
     *
     */
    constexpr tensor_view() = default;

    /**
     * @brief Creates a borrowed view over a compatible tensor-like owner.
     *
     * This reads the source object's data pointer, shape, and domain to build a
     * non-owning contiguous tensor view.
     *
     * @param tensor Tensor-like object providing contiguous storage, shape, and
     * domain metadata.
     */
    constexpr explicit tensor_view(tensor_type auto& tensor)
        requires compatible_tensor_type<decltype(tensor), value_type, shape_type, Domain> &&
                 std::convertible_to<decltype(tensor.data()), element_type*>
        : tensor_view(tensor.data(), tensor.size(), tensor.shape(), tensor.domain()) {}

    /**
     * @brief Creates a contiguous view from a contiguous sized range and shape.
     *
     * @param range Contiguous sized range covering the borrowed value buffer.
     * @param shape Logical extents used to interpret the range.
     * @param domain Numerical domain metadata associated with the values.
     */
    constexpr tensor_view(auto&& range, const shape_type& shape, Domain domain = {})
        requires contiguous_tensor_source<decltype(range), value_type> &&
                 std::convertible_to<decltype(std::ranges::data(range)), element_type*>
        : tensor_view(std::ranges::data(range), std::ranges::size(range), shape, domain) {}

    /**
     * @brief Creates a one-dimensional view over an entire contiguous sized
     * range.
     *
     * @param range Contiguous sized range covering the one-dimensional buffer.
     * @param domain Numerical domain metadata associated with the values.
     */
    constexpr explicit tensor_view(auto&& range, Domain domain = {})
        requires(rank_value == 1 && contiguous_tensor_source<decltype(range), value_type> &&
                 std::convertible_to<decltype(std::ranges::data(range)), element_type*>)
        : tensor_view(std::forward<decltype(range)>(range), shape_type(std::ranges::size(range)),
                      domain) {}

    /**
     * @brief Returns the view rank as a compile-time constant.
     *
     * This exposes how many axes the view shape contains.
     *
     * @return Number of logical axes in the view type.
     */
    [[nodiscard]] static consteval std::size_t rank() { return rank_value; }

    /**
     * @brief Returns a pointer to the first borrowed element.
     *
     * This exposes the base address used by the internal `mdspan`.
     *
     * @return Pointer to the first borrowed element.
     */
    [[nodiscard]] constexpr decltype(auto) data(this auto& self) {
        using pointer_type =
            std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(self)>>,
                               std::add_const_t<element_type>*, element_type*>;
        return static_cast<pointer_type>(self.view_v.data_handle());
    }

    /**
     * @brief Returns an iterator to the first borrowed element.
     *
     * This exposes flat storage-order iteration over the contiguous view.
     *
     * @return Iterator to the first borrowed element.
     */
    [[nodiscard]] constexpr auto begin(this auto& self) { return self.data(); }

    /**
     * @brief Returns an iterator one past the last borrowed element.
     *
     * This exposes flat storage-order iteration over the contiguous view.
     *
     * @return Iterator one past the last borrowed element.
     */
    [[nodiscard]] constexpr auto end(this auto& self) { return self.data() + self.size(); }

    /**
     * @brief Returns the logical shape metadata of the view.
     *
     * This exposes the shape stored when the view was constructed.
     *
     * @return Shape object describing the view extents.
     */
    [[nodiscard]] constexpr const shape_type& shape() const { return shape_v; }

    /**
     * @brief Returns the numerical domain metadata associated with the view.
     *
     * This exposes how the borrowed values should be interpreted.
     *
     * @return Reference to the domain metadata object.
     */
    [[nodiscard]] constexpr decltype(auto) domain(this auto& self) { return (self.domain_v); }

    /**
     * @brief Returns the logical element count of the view.
     *
     * This multiplies the extents of the current shape.
     *
     * @return Number of logical elements accessible through the view.
     */
    [[nodiscard]] constexpr size_type size() const {
        return detail::logical_element_count(shape());
    }

    /**
     * @brief Reports whether the view contains no logical elements.
     *
     * This checks whether the logical element count is zero.
     *
     * @return `true` when the view exposes zero logical elements.
     */
    [[nodiscard]] constexpr bool empty() const { return size() == 0; }

    template <class... Indices>
        requires(sizeof...(Indices) == rank_value &&
                 (std::convertible_to<Indices, size_type> && ...))
    /**
     * @brief Accesses one tensor element using multidimensional indices.
     *
     * This forwards the supplied logical indices into the internal `mdspan`
     * mapping.
     *
     * @param indices One index per tensor axis.
     * @return Reference-like access to the selected element.
     */
    [[nodiscard]] constexpr decltype(auto) operator[](this auto& self, Indices... indices) {
        return self.view()[static_cast<size_type>(indices)...];
    }

private:
    constexpr tensor_view(element_type* data, size_type available_size, const shape_type& shape,
                          Domain domain) pre(available_size >= detail::logical_element_count(shape))
        : view_v(data, detail::make_extents(shape)), shape_v(shape), domain_v(domain) {}

    [[nodiscard]] constexpr auto view(this auto& self) {
        using view_element_type =
            std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(self)>>,
                               std::add_const_t<element_type>, element_type>;
        return detail::md::mdspan<view_element_type, dynamic_extents<rank_value>>(
            self.view_v.data_handle(), detail::make_extents(self.shape_v));
    }

    mdspan_type view_v{};
    shape_type shape_v{};
    Domain domain_v{};
};

template <tensor_type TensorLike>
tensor_view(TensorLike&)
    -> tensor_view<std::remove_pointer_t<decltype(std::declval<TensorLike&>().data())>,
                   typename std::remove_cvref_t<TensorLike>::domain_type,
                   typename std::remove_cvref_t<TensorLike>::shape_type>;

template <class Range>
    requires contiguous_tensor_source<Range, std::remove_cv_t<detail::range_element_t<Range>>>
tensor_view(Range&&)
    -> tensor_view<detail::range_element_t<Range>,
                   domain::no_quantization<std::remove_cv_t<detail::range_element_t<Range>>>,
                   tensor_shape<1>>;

template <class Range, class Domain>
    requires contiguous_tensor_source<Range, typename std::remove_cvref_t<Domain>::value_type> &&
             (!defines_rank<Domain>)
tensor_view(Range&&, Domain)
    -> tensor_view<detail::range_element_t<Range>, std::remove_cvref_t<Domain>, tensor_shape<1>>;

template <class Range, class Shape>
    requires defines_rank<Shape> &&
             contiguous_tensor_source<Range, std::remove_cv_t<detail::range_element_t<Range>>>
tensor_view(Range&&, Shape)
    -> tensor_view<detail::range_element_t<Range>,
                   domain::no_quantization<std::remove_cv_t<detail::range_element_t<Range>>>,
                   std::remove_cvref_t<Shape>>;

template <class Range, class Shape, class Domain>
    requires defines_rank<Shape> && has_value_type<Domain> &&
             contiguous_tensor_source<Range, typename std::remove_cvref_t<Domain>::value_type>
tensor_view(Range&&, Shape, Domain)
    -> tensor_view<detail::range_element_t<Range>, std::remove_cvref_t<Domain>,
                   std::remove_cvref_t<Shape>>;

template <class View>
concept tensor_view_type = specialization_of<View, tensor_view>;

} // namespace esp::tensor

template <class T, class Domain, class Shape>
inline constexpr bool std::ranges::enable_view<esp::tensor::tensor_view<T, Domain, Shape>> = true;

template <class T, class Domain, class Shape>
inline constexpr bool
    std::ranges::enable_borrowed_range<esp::tensor::tensor_view<T, Domain, Shape>> = true;
