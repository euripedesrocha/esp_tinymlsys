#pragma once

/**
 * @file
 * @brief Selects an available `mdspan` implementation behind a stable alias.
 *
 * This header normalizes access to either the standard or experimental
 * `mdspan` namespace and guards one known macro collision.
 *
 * Once the compiler version gets mdspan implementation this could be removed
 */

#include <memory>
#include <version>

#if defined(_U)
#define ESP_TENSOR_RESTORE_MDSPAN_CTYPE_U 1
#pragma push_macro("_U")
#undef _U
#endif

#if defined(__cpp_lib_mdspan)
#include <mdspan>
namespace esp::tensor::detail {
namespace md = std;
}
#elif __has_include(<experimental/mdspan>)
#include <experimental/mdspan>
namespace esp::tensor::detail {
namespace md = std::experimental;
}
#else
#error "No mdspan implementation found"
#endif

#if defined(ESP_TENSOR_RESTORE_MDSPAN_CTYPE_U)
#pragma pop_macro("_U")
#undef ESP_TENSOR_RESTORE_MDSPAN_CTYPE_U
#endif
