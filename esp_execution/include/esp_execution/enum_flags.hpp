// SPDX-FileCopyrightText: 2026 Euripedes Rocha
// SPDX-License-Identifier: Apache-2.0

#pragma once

#include <type_traits>
#include <utility>

namespace esp::execution::enum_flags {

// Import the operators with using-declarations in the enum's namespace for ADL.
template <class Enum>
concept scoped_enum = std::is_scoped_enum_v<Enum>;

template <scoped_enum Enum>
[[nodiscard]] constexpr auto operator|(Enum left, Enum right) noexcept -> Enum {
    return static_cast<Enum>(std::to_underlying(left) | std::to_underlying(right));
}

template <scoped_enum Enum>
[[nodiscard]] constexpr auto operator&(Enum left, Enum right) noexcept -> Enum {
    return static_cast<Enum>(std::to_underlying(left) & std::to_underlying(right));
}

} // namespace esp::execution::enum_flags
