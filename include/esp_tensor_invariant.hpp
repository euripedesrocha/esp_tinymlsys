#pragma once

#include <stdexcept>

namespace esp::tensor::detail {

/**
 * @brief Verifies a required condition for tensor construction or use.
 *
 * This throws an `std::invalid_argument` when exceptions are enabled, or
 * terminates otherwise.
 *
 */
inline void validate_invariant(bool verified, const char* message) {
    if (!verified) {
#if defined(__cpp_exceptions)
        throw std::invalid_argument(message);
#else
        (void)message;
        std::terminate();
#endif
    }
}

} // namespace esp::tensor::detail
