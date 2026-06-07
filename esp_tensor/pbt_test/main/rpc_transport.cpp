/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "rpc_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "driver/usb_serial_jtag.h"
#include "freertos/FreeRTOS.h"
#include "portmacro.h"

namespace rpc {
namespace {

// USB Serial/JTAG is a byte stream shared with reset/boot logs. The request
// magic lets the target resynchronize before the next complete frame instead of
// treating stray bytes as a header. The response magic lets the host reject any
// unexpected target output before decoding the payload.
constexpr std::uint32_t request_magic = 0x31514352;
constexpr std::uint32_t response_magic = 0x31505352;

constexpr TickType_t byte_timeout = pdMS_TO_TICKS(100);
constexpr TickType_t frame_timeout = pdMS_TO_TICKS(1000);

// Keep this layout synchronized with RpcClient.call() in pbt_test/rpc/client.py:
// struct.pack("<IHI", request_magic, method_size, payload_size).
struct request_header {
    std::uint32_t magic;
    std::uint16_t method_size;
    std::uint32_t payload_size;
} __attribute__((packed));
static_assert(sizeof(request_header) == 10, "request_header must be 10 bytes");

// Keep this layout synchronized with RpcClient.call() in pbt_test/rpc/client.py:
// struct.unpack("<IBI", response_header).
struct response_header {
    std::uint32_t magic;
    std::uint8_t status;
    std::uint32_t payload_size;
} __attribute__((packed));
static_assert(sizeof(response_header) == 9, "response_header must be 9 bytes");

struct request_frame {
    std::vector<std::uint8_t> method;
    std::vector<std::uint8_t> payload;
};

[[nodiscard]] bool read_exact(std::span<std::byte> buffer, TickType_t timeout = frame_timeout) {
    auto* current = reinterpret_cast<std::uint8_t*>(buffer.data());
    std::size_t remaining = buffer.size();
    while (remaining > 0) {
        const auto chunk = std::min<std::size_t>(remaining, 4096);
        const auto bytes_read =
            usb_serial_jtag_read_bytes(current, static_cast<std::uint32_t>(chunk), timeout);
        if (bytes_read <= 0) {
            return false;
        }
        current += bytes_read;
        remaining -= static_cast<std::size_t>(bytes_read);
    }
    return true;
}

[[nodiscard]] request_header read_next_request_header() {
    std::uint32_t window = 0;

    for (;;) {
        std::uint8_t next = 0;
        const auto bytes_read = usb_serial_jtag_read_bytes(&next, 1, byte_timeout);
        if (bytes_read <= 0) {
            continue;
        }

        window = (window >> 8) | (static_cast<std::uint32_t>(next) << 24);
        if (window != request_magic) {
            continue;
        }

        request_header header{.magic = request_magic, .method_size = 0, .payload_size = 0};
        auto rest = std::as_writable_bytes(std::span{std::addressof(header), 1})
                        .subspan(sizeof(header.magic));
        if (read_exact(rest)) {
            return header;
        }

        window = 0;
    }
}

[[nodiscard]] bool discard_exact(std::size_t size) {
    std::array<std::byte, 256> buffer{};
    while (size > 0) {
        const auto chunk = std::min(size, buffer.size());
        if (!read_exact(std::span{buffer}.first(chunk))) {
            return false;
        }
        size -= chunk;
    }
    return true;
}

[[nodiscard]] bool write_exact(std::span<const std::byte> buffer) {
    const auto* current = reinterpret_cast<const std::uint8_t*>(buffer.data());
    std::size_t remaining = buffer.size();
    while (remaining > 0) {
        const auto chunk = std::min<std::size_t>(remaining, 4096);
        const auto bytes_written = usb_serial_jtag_write_bytes(current, chunk, pdMS_TO_TICKS(1000));
        if (bytes_written <= 0) {
            // Timeout, disconnect, or host reset. The server loop treats this as
            // recoverable and waits for the next synchronized request frame.
            return false;
        }
        current += bytes_written;
        remaining -= static_cast<std::size_t>(bytes_written);
    }
    return true;
}

[[nodiscard]] result make_error(status code, std::string_view message) {
    return {
        .code = code,
        .payload = std::vector<std::uint8_t>(message.begin(), message.end()),
    };
}

[[nodiscard]] std::expected<request_frame, result> read_request_frame() {
    for (;;) {
        const auto header = read_next_request_header();

        if (header.method_size > max_method_size) {
            if (!discard_exact(static_cast<std::size_t>(header.method_size) +
                               header.payload_size)) {
                continue;
            }
            return std::unexpected(make_error(status::bad_frame, "method name too long"));
        }

        if (header.payload_size > max_payload_size) {
            if (!discard_exact(static_cast<std::size_t>(header.method_size) +
                               header.payload_size)) {
                continue;
            }
            return std::unexpected(make_error(status::bad_frame, "payload too large"));
        }

        request_frame frame{
            .method = std::vector<std::uint8_t>(header.method_size),
            .payload = std::vector<std::uint8_t>(header.payload_size),
        };

        if (!read_exact(std::as_writable_bytes(std::span{frame.method})) ||
            !read_exact(std::as_writable_bytes(std::span{frame.payload}))) {
            continue;
        }

        return frame;
    }
}

[[nodiscard]] const command* find_command(std::span<const command> commands,
                                          std::string_view name) {
    const auto command = std::ranges::find(commands, name, &command::name);
    return command == commands.end() ? nullptr : std::addressof(*command);
}

bool write_response(const result& result) {
    const response_header header = {
        .magic = response_magic,
        .status = static_cast<std::uint8_t>(std::to_underlying(result.code)),
        .payload_size = static_cast<std::uint32_t>(result.payload.size()),
    };

    if (!write_exact(std::as_bytes(std::span{std::addressof(header), 1}))) {
        return false;
    }
    if (!write_exact(std::as_bytes(std::span{result.payload}))) {
        return false;
    }
    (void)usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(100));
    return true;
}

} // namespace

void run_server(std::span<const command> commands) {
    for (;;) {
        auto frame = read_request_frame();
        if (!frame) {
            if (!write_response(frame.error())) {
                continue;
            }
            continue;
        }

        const std::string_view method{reinterpret_cast<const char*>(frame->method.data()),
                                      frame->method.size()};

        const auto* cmd = find_command(commands, method);
        if (cmd == nullptr) {
            if (!write_response(make_error(status::unknown_method, "unknown rpc method"))) {
                continue;
            }
            continue;
        }

        const auto response = cmd->fn(frame->payload);
        if (!write_response(response)) {
            continue;
        }
    }
}

} // namespace rpc
