// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <concepts>
#include <cstddef>
#include <functional>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

#include "esp_execution/concepts.hpp"

namespace esp::execution::detail {

template <operation_state Operation> class operation_state_storage {
public:
    constexpr operation_state_storage() noexcept = default;

    operation_state_storage(const operation_state_storage&) = delete;
    operation_state_storage(operation_state_storage&&) = delete;
    operation_state_storage& operator=(const operation_state_storage&) = delete;
    operation_state_storage& operator=(operation_state_storage&&) = delete;

    ~operation_state_storage() { reset(); }

    template <std::invocable Factory>
        requires std::same_as<std::invoke_result_t<Factory>, Operation>
    Operation& emplace_from(Factory&& factory) noexcept(std::is_nothrow_invocable_v<Factory>) {
        reset();
        ::new (static_cast<void*>(storage_)) Operation(std::invoke(std::forward<Factory>(factory)));
        engaged_ = true;
        return *pointer();
    }

private:
    void reset() noexcept {
        if (engaged_) {
            std::destroy_at(pointer());
            engaged_ = false;
        }
    }

    Operation* pointer() noexcept { return std::launder(reinterpret_cast<Operation*>(storage_)); }

    alignas(Operation) std::byte storage_[sizeof(Operation)];
    bool engaged_ = false;
};

} // namespace esp::execution::detail
