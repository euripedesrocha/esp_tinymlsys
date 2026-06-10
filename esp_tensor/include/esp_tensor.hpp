#pragma once

#include <concepts>
#include <memory>
#include <memory_resource>
#include <type_traits>
#include <utility>

#include "esp_tensor_concepts.hpp"
#include "esp_tensor_domain.hpp"
#include "esp_tensor_shape.hpp"

namespace esp::tensor {

/**
 * @brief Owns a contiguous tensor buffer together with shape and domain
 * metadata.
 *
 * A tensor is a multidimensional data structure.
 * A contiguous memory paired with shape, quantization numerical domain metadata
 * This class also is PMR aware to allow control over memory resources.
 *
 */
template <has_value_type Domain = domain::no_quantization<>, defines_rank Shape = tensor_shape<1>>
class tensor {
public:
    using domain_type = Domain;
    using shape_type = Shape;
    using size_type = typename std::pmr::vector<typename domain_type::value_type>::size_type;

    static constexpr std::size_t rank_value = shape_type::rank_value;

    /**
     * @brief Allocates PMR-backed storage for a tensor with the given shape and
     * domain.
     *
     * This creates the contiguous element buffer that will hold the tensor
     * values and binds it to the supplied shape and numerical domain.
     *
     * @param shape Logical extents of the tensor to allocate.
     * @param domain Numerical domain used to interpret the stored values.
     * @param resource PMR resource used to allocate the contiguous storage.
     */
    explicit tensor(const shape_type& shape, Domain domain = {},
                    std::pmr::memory_resource* resource = std::pmr::get_default_resource())
        : storage_data(resource), shape_v(shape), domain_v(domain) {
        storage_data.resize(detail::logical_element_count(shape_v));
    }

    /**
     * @brief Copies tensor storage using the source tensor's memory resource.
     *
     * `std::pmr::vector` copy construction selects the current default resource
     * by default. Tensor copies preserve the source resource instead, so
     * alignment and heap-capability choices remain stable across copies.
     */
    tensor(const tensor& other)
        : storage_data(other.storage_data, other.storage_data.get_allocator()),
          shape_v(other.shape_v), domain_v(other.domain_v) {}

    /**
     * @brief Move construction transfers tensor storage and metadata.
     */
    tensor(tensor&&) noexcept = default;

    /**
     * @brief Copy assignment preserves the source tensor's memory resource.
     */
    tensor& operator=(const tensor& other) {
        if (this != &other) {
            auto copy = tensor(other);
            std::destroy_at(this);
            std::construct_at(this, std::move(copy));
        }

        return *this;
    }

    /**
     * @brief Move assignment transfers tensor storage and metadata.
     */
    tensor& operator=(tensor&&) noexcept = default;

    /**
     * @brief Returns the tensor rank.
     *
     * This exposes how many axes the tensor shape contains.
     *
     * @return Number of logical axes in the tensor type.
     */
    [[nodiscard]] static consteval std::size_t rank() { return rank_value; }

    /**
     * @brief Returns a pointer to the tensor's contiguous storage buffer.
     *
     * @return Pointer to the first stored element.
     */
    [[nodiscard]] constexpr decltype(auto) data(this auto& self) {
        return self.storage_data.data();
    }

    /**
     * @brief Returns an iterator to the first stored element.
     *
     * @return Iterator to the first element in storage.
     */
    [[nodiscard]] constexpr auto begin(this auto& self) { return self.storage_data.begin(); }

    /**
     * @brief Returns an iterator one past the last stored element.
     *
     * @return Iterator one past the last element in storage.
     */
    [[nodiscard]] constexpr auto end(this auto& self) { return self.storage_data.end(); }

    /**
     * @brief Returns the tensor shape metadata.
     *
     * This exposes the logical extents used to interpret the flat storage.
     *
     * @return Reference to the tensor shape object.
     */
    [[nodiscard]] constexpr const shape_type& shape() const { return shape_v; }

    /**
     * @brief Returns the tensor's numerical domain metadata.
     *
     * This exposes the domain object that defines both the scalar
     * representation and any extra numerical parameters such as quantization
     * scale or fixed-point layout.
     *
     * @return Reference to the tensor domain object.
     */
    [[nodiscard]] constexpr decltype(auto) domain(this auto& self) { return (self.domain_v); }

    template <class... Indices>
        requires(sizeof...(Indices) == rank_value &&
                 (std::convertible_to<Indices, size_type> && ...))
    /**
     * @brief Accesses one tensor element using multidimensional indices.
     *
     * This forwards the supplied logical indices into the tensor's internal
     * multidimensional view.
     *
     * @param indices One index per tensor axis.
     * @return Reference-like access to the selected tensor element.
     */
    [[nodiscard]] constexpr decltype(auto) operator[](this auto& self, Indices... indices) {
        return self.view()[static_cast<size_type>(indices)...];
    }

    /**
     * @brief Reports whether the tensor contains no logical elements.
     *
     * This checks whether the owned contiguous storage is empty.
     *
     * @return `true` when the tensor stores zero logical elements.
     */
    [[nodiscard]] constexpr bool empty() const { return size() == 0; }

    /**
     * @brief Returns the number of stored logical elements.
     *
     * This reports the flat element count of the owned storage.
     *
     * @return Number of elements stored by the tensor.
     */
    [[nodiscard]] constexpr size_type size() const { return storage_data.size(); }

private:
    /**
     * @brief Builds a multidimensional view over the owned storage.
     *
     * This reinterprets the owned PMR vector as an `mdspan` using the current
     * tensor shape, with constness derived from the tensor object itself.
     *
     * @return `mdspan` over the tensor storage with element constness matching
     * the tensor object.
     */
    [[nodiscard]] constexpr auto view(this auto& self) {
        using element_type =
            std::conditional_t<std::is_const_v<std::remove_reference_t<decltype(self)>>,
                               const typename domain_type::value_type,
                               typename domain_type::value_type>;
        return detail::md::mdspan<element_type, dynamic_extents<rank_value>>(
            self.data(), detail::make_extents(self.shape_v));
    }

    std::pmr::vector<typename domain_type::value_type> storage_data;
    shape_type shape_v{};
    Domain domain_v{};
};

template <defines_rank Shape>
tensor(Shape) -> tensor<domain::no_quantization<>, std::remove_cvref_t<Shape>>;

template <defines_rank Shape, has_value_type Domain>
tensor(Shape, Domain) -> tensor<std::remove_cvref_t<Domain>, std::remove_cvref_t<Shape>>;

template <defines_rank Shape, has_value_type Domain>
tensor(Shape, Domain, std::pmr::memory_resource*)
    -> tensor<std::remove_cvref_t<Domain>, std::remove_cvref_t<Shape>>;

} // namespace esp::tensor
