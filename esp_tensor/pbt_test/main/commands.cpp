/*
 * SPDX-FileCopyrightText: 2026
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */
#include "rpc_transport.hpp"
#include "tensor_rpc_protocol.hpp"

#include <array>
#include <cstdint>
#include <expected>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "esp_memory_resource.hpp"
#include "esp_tensor.hpp"
#include "esp_tensor_arith.hpp"
#include "esp_tensor_arith_affine.hpp"
#include "esp_tensor_arith_fixed_point.hpp"
#include "esp_tensor_ops.hpp"
#include "esp_tensor_view.hpp"

using namespace esp::tensor;

namespace {

namespace tensor_rpc = pbt::tensor_rpc;

template <class T> using command_result = tensor_rpc::command_result<T>;

[[nodiscard]] rpc::result handle_ping(std::span<const std::uint8_t> request) {
    if (!request.empty()) {
        return tensor_rpc::make_error(rpc::status::bad_frame, "ping request payload must be empty");
    }
    constexpr std::string_view pong = "pong";
    return {
        .code = rpc::status::ok,
        .payload = tensor_rpc::as_bytes(pong),
    };
}

struct binary_elementwise_parameters {
    tensor_rpc::tensor_param lhs;
    tensor_rpc::tensor_param rhs;
    tensor_rpc::tensor_param out;
};

template <class Tensor> [[nodiscard]] rpc::result encode_tensor_payload(Tensor& tensor) {
    const auto out_span = std::span{tensor.data(), tensor.size()};
    return {
        .code = rpc::status::ok,
        .payload = std::vector<std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(out_span.data()),
            reinterpret_cast<const std::uint8_t*>(out_span.data()) + out_span.size_bytes()),
    };
}

[[nodiscard]] command_result<rpc::result>
invoke_binary_elementwise(binary_elementwise_parameters params, auto operation) {
    return std::visit(
        [&params, &operation](auto& lhs) -> command_result<rpc::result> {
            using tensor_type = std::remove_cvref_t<decltype(lhs)>;
            auto* rhs = std::get_if<tensor_type>(&params.rhs);
            auto* out = std::get_if<tensor_type>(&params.out);
            if (rhs == nullptr || out == nullptr) {
                return std::unexpected(tensor_rpc::make_error(
                    rpc::status::bad_frame, "prepared operand tensor types must match"));
            }

            try {
                operation(tensor_view(lhs), tensor_view(*rhs), tensor_view(*out));
            } catch (const std::invalid_argument& error) {
                return std::unexpected(tensor_rpc::make_error(rpc::status::error, error.what()));
            }
            return encode_tensor_payload(*out);
        },
        params.lhs);
}

[[nodiscard]] command_result<binary_elementwise_parameters>
prepare_add_parameters(tensor_rpc::tensor_parameters params, std::pmr::memory_resource* mem) {
    if (params.inputs.size() != 2) {
        return std::unexpected(
            tensor_rpc::make_error(rpc::status::bad_frame, "add requires 2 input tensors"));
    }

    return std::visit(
        [&](auto& lhs) -> command_result<binary_elementwise_parameters> {
            using tensor_type = std::remove_cvref_t<decltype(lhs)>;
            auto* rhs = std::get_if<tensor_type>(&params.inputs[1]);
            if (rhs == nullptr) {
                return std::unexpected(tensor_rpc::make_error(rpc::status::bad_frame,
                                                              "operand tensor types must match"));
            }
            if (lhs.shape() != rhs->shape()) {
                return std::unexpected(
                    tensor_rpc::make_error(rpc::status::bad_frame, "operand shapes must match"));
            }

            auto out_domain = lhs.domain();
            if constexpr (requires {
                              out_domain.scale;
                              out_domain.zero_point;
                          }) {
                out_domain.scale = params.output_affine.scale;
                out_domain.zero_point = params.output_affine.zero_point;
            }

            tensor_type out(lhs.shape(), out_domain, mem);
            return binary_elementwise_parameters{
                .lhs = std::move(lhs),
                .rhs = std::move(*rhs),
                .out = tensor_rpc::tensor_param{std::move(out)},
            };
        },
        params.inputs[0]);
}

[[nodiscard]] rpc::result handle_add(std::span<const std::uint8_t> request) {
    auto caps = esp::memory::capabilities::internal | esp::memory::capabilities::addressable_8bit |
                esp::memory::accelerator_capabilities();
    // Alignment is enforced by the allocator used to materialize the decoded tensors.
    // Arithmetic kernels still validate the resulting view alignment before selecting
    // an accelerated path.
    esp::memory::accelerator_resource mem{caps};

    auto result =
        tensor_rpc::decode_tensor_request(request)
            .and_then([&](const tensor_rpc::tensor_request& decoded) {
                return tensor_rpc::build_tensor_parameters(decoded, &mem);
            })
            .and_then([&](tensor_rpc::tensor_parameters params) {
                return prepare_add_parameters(std::move(params), &mem);
            })
            .and_then([](binary_elementwise_parameters params) {
                return invoke_binary_elementwise(
                    std::move(params), [](auto lhs, auto rhs, auto out) { add(lhs, rhs, out); });
            });

    return result.has_value() ? result.value() : result.error();
}

} // namespace

constexpr std::array commands = {
    rpc::command{.name = "rpc.ping", .fn = &handle_ping},
    rpc::command{.name = "arith.add", .fn = &handle_add},
};

std::span<const rpc::command> get_commands() { return commands; }
