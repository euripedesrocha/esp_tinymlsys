#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace rpc {

inline constexpr std::size_t max_method_size = 64;
// Test harness budget, not a production protocol limit. Current PBT add cases
// generate up to 64 elements per input, so 16 KiB leaves room for the request
// header, two operands, and future small elementwise operators while keeping the
// USB Serial/JTAG buffers bounded.
inline constexpr std::size_t max_payload_size = 16 * 1024;
inline constexpr std::size_t usb_serial_tx_buffer_size = max_payload_size;
inline constexpr std::size_t usb_serial_rx_buffer_size = max_payload_size;

// Explicit values make the status byte a stable wire ABI shared with
// pbt_test/rpc/client.py::RpcStatus.
enum class status : std::uint8_t {
    ok = 0,
    error = 1,
    bad_frame = 2,
    unknown_method = 3,
};

struct result {
    status code = status::ok;
    std::vector<std::uint8_t> payload;
};

struct command {
    std::string_view name;
    result (*fn)(std::span<const std::uint8_t>);
};

void run_server(std::span<const command> commands);

} // namespace rpc
