#pragma once

#include "rpc_transport.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <expected>
#include <memory_resource>
#include <span>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "esp_tensor.hpp"
#include "esp_tensor_domain.hpp"

namespace pbt::tensor_rpc {

using namespace esp::tensor;

[[nodiscard]] inline auto as_bytes(std::string_view message) {
    return std::vector<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(message.data()),
                                     reinterpret_cast<const std::uint8_t*>(message.data()) +
                                         message.size());
}

[[nodiscard]] inline rpc::result make_error(rpc::status code, std::string_view message) {
    return {
        .code = code,
        .payload = as_bytes(message),
    };
}

template <class T> using command_result = std::expected<T, rpc::result>;

enum class wire_dtype : std::uint8_t {
    float32 = 0,
    int8 = 1,
    int16 = 2,
};

enum class wire_domain_type : std::uint8_t {
    no_quantization = 0,
    fixed_point = 1,
    affine = 2,
};

using fixed_point_s16 = domain::fixed_point<std::int16_t>;
using fixed_point_s8 = domain::fixed_point<std::int8_t>;
using affine_s8 = domain::affine<>;
using no_quant = domain::no_quantization<float>;

using tensor_param =
    std::variant<tensor<fixed_point_s16, tensor_shape<1>>, tensor<fixed_point_s16, tensor_shape<2>>,
                 tensor<fixed_point_s16, tensor_shape<3>>, tensor<fixed_point_s16, tensor_shape<4>>,
                 tensor<fixed_point_s8, tensor_shape<1>>, tensor<fixed_point_s8, tensor_shape<2>>,
                 tensor<fixed_point_s8, tensor_shape<3>>, tensor<fixed_point_s8, tensor_shape<4>>,
                 tensor<affine_s8, tensor_shape<1>>, tensor<affine_s8, tensor_shape<2>>,
                 tensor<affine_s8, tensor_shape<3>>, tensor<affine_s8, tensor_shape<4>>,
                 tensor<no_quant, tensor_shape<1>>, tensor<no_quant, tensor_shape<2>>,
                 tensor<no_quant, tensor_shape<3>>, tensor<no_quant, tensor_shape<4>>>;

struct affine_header {
    float scale = 1.0F;
    std::int32_t zero_point = 0;
};

struct tensor_parameters {
    affine_header output_affine;
    std::vector<tensor_param> inputs;
};

struct operand_header {
    std::uint8_t rank;
    std::vector<std::size_t> shape;
};

struct tensor_wire_header {
    wire_dtype dtype;
    wire_domain_type domain_type;
    affine_header output_affine;
    std::vector<operand_header> operands;
    std::vector<affine_header> operand_affines;
};

struct tensor_param_desc {
    wire_dtype dtype;
    wire_domain_type domain_type;
    affine_header affine;
    std::span<const std::size_t> shape;
    std::span<const std::uint8_t> payload;
};

struct tensor_request_operand {
    operand_header header;
    affine_header affine;
    std::span<const std::uint8_t> payload;
};

struct tensor_request {
    wire_dtype dtype;
    wire_domain_type domain_type;
    affine_header output_affine;
    std::vector<tensor_request_operand> operands;
};

[[nodiscard]] inline command_result<affine_header>
decode_affine_header(std::span<const std::uint8_t> data, std::size_t& offset) {
    constexpr std::size_t affine_header_size = sizeof(float) + sizeof(std::int32_t);
    if (data.size() < offset + affine_header_size) {
        return std::unexpected(make_error(rpc::status::bad_frame, "missing affine metadata"));
    }

    affine_header header{};
    std::memcpy(&header.scale, data.data() + offset, sizeof(float));
    offset += sizeof(float);
    std::memcpy(&header.zero_point, data.data() + offset, sizeof(std::int32_t));
    offset += sizeof(std::int32_t);
    return header;
}

[[nodiscard]] inline command_result<wire_dtype> parse_wire_dtype(std::uint8_t value) {
    switch (value) {
    case std::to_underlying(wire_dtype::float32):
        return wire_dtype::float32;
    case std::to_underlying(wire_dtype::int8):
        return wire_dtype::int8;
    case std::to_underlying(wire_dtype::int16):
        return wire_dtype::int16;
    default:
        return std::unexpected(make_error(rpc::status::bad_frame, "unsupported tensor dtype"));
    }
}

[[nodiscard]] inline command_result<wire_domain_type> parse_wire_domain_type(std::uint8_t value) {
    switch (value) {
    case std::to_underlying(wire_domain_type::no_quantization):
        return wire_domain_type::no_quantization;
    case std::to_underlying(wire_domain_type::fixed_point):
        return wire_domain_type::fixed_point;
    case std::to_underlying(wire_domain_type::affine):
        return wire_domain_type::affine;
    default:
        return std::unexpected(make_error(rpc::status::bad_frame, "unsupported tensor domain"));
    }
}

[[nodiscard]] inline command_result<tensor_wire_header>
decode_tensor_header(std::span<const std::uint8_t> data) {
    if (data.size() < 3) {
        return std::unexpected(make_error(rpc::status::bad_frame, "tensor header too small"));
    }

    auto dtype = parse_wire_dtype(data[0]);
    if (!dtype) {
        return std::unexpected(dtype.error());
    }

    auto domain_type = parse_wire_domain_type(data[1]);
    if (!domain_type) {
        return std::unexpected(domain_type.error());
    }

    const std::uint8_t num_inputs = data[2];

    tensor_wire_header header{
        .dtype = dtype.value(),
        .domain_type = domain_type.value(),
        .output_affine = {},
        .operands = {},
        .operand_affines = {},
    };

    std::size_t offset = 3;
    if (header.domain_type == wire_domain_type::affine) {
        auto output_affine = decode_affine_header(data, offset);
        if (!output_affine) {
            return std::unexpected(output_affine.error());
        }
        header.output_affine = output_affine.value();
        header.operand_affines.reserve(num_inputs);
    }

    header.operands.reserve(num_inputs);

    for (std::uint8_t i = 0; i < num_inputs; ++i) {
        if (data.size() < offset + 1) {
            return std::unexpected(make_error(rpc::status::bad_frame, "missing operand rank"));
        }

        const std::uint8_t rank = data[offset];
        offset += 1;

        const std::size_t shape_bytes = rank * sizeof(std::uint32_t);
        if (data.size() < offset + shape_bytes) {
            return std::unexpected(make_error(rpc::status::bad_frame, "missing operand shape"));
        }

        operand_header operand{.rank = rank, .shape = {}};
        operand.shape.resize(rank);
        for (std::size_t j = 0; j < rank; ++j) {
            std::uint32_t dim = 0;
            std::memcpy(&dim, data.data() + offset + (j * sizeof(std::uint32_t)),
                        sizeof(std::uint32_t));
            operand.shape[j] = dim;
        }
        offset += shape_bytes;
        header.operands.push_back(std::move(operand));

        if (header.domain_type == wire_domain_type::affine) {
            auto operand_affine = decode_affine_header(data, offset);
            if (!operand_affine) {
                return std::unexpected(operand_affine.error());
            }
            header.operand_affines.push_back(operand_affine.value());
        }
    }

    return header;
}

[[nodiscard]] inline std::size_t get_header_size(const tensor_wire_header& header) {
    std::size_t prefix = 3; // dtype + domain_type + num_inputs
    if (header.domain_type == wire_domain_type::affine) {
        prefix += sizeof(float) + sizeof(std::int32_t); // output scale + zero point
    }
    for (const auto& operand : header.operands) {
        prefix += 1 + (operand.rank * sizeof(std::uint32_t)); // rank byte + shape
        if (header.domain_type == wire_domain_type::affine) {
            prefix += sizeof(float) + sizeof(std::int32_t); // operand scale + zero point
        }
    }
    return prefix;
}

template <class Shape> [[nodiscard]] Shape make_shape(std::span<const std::size_t> extents) {
    std::array<std::size_t, Shape::rank_value> values{};
    std::ranges::copy(extents, values.begin());
    return Shape{values};
}

[[nodiscard]] inline std::size_t element_count(std::span<const std::size_t> shape) {
    std::size_t number_of_elements = 1;
    for (auto dim : shape) {
        number_of_elements *= dim;
    }
    return number_of_elements;
}

[[nodiscard]] inline std::size_t dtype_byte_size(wire_dtype dtype) {
    switch (dtype) {
    case wire_dtype::float32:
        return sizeof(float);
    case wire_dtype::int8:
        return sizeof(std::int8_t);
    case wire_dtype::int16:
        return sizeof(std::int16_t);
    }
    std::unreachable();
}

template <class Domain, class Shape>
[[nodiscard]] command_result<tensor<Domain, Shape>>
build_tensor_param(const tensor_param_desc& desc, std::pmr::memory_resource* mem) {
    const auto shape = make_shape<Shape>(desc.shape);
    Domain domain{};
    if constexpr (requires(Domain item) {
                      item.scale;
                      item.zero_point;
                  }) {
        domain.scale = desc.affine.scale;
        domain.zero_point = desc.affine.zero_point;
    }

    tensor<Domain, Shape> param(shape, domain, mem);

    if (!desc.payload.empty()) {
        std::memcpy(param.data(), desc.payload.data(), desc.payload.size_bytes());
    }

    return param;
}

[[nodiscard]] inline command_result<void> validate_tensor_desc(const tensor_param_desc& desc) {
    const std::size_t byte_size = element_count(desc.shape) * dtype_byte_size(desc.dtype);
    if (desc.payload.size() != byte_size) {
        return std::unexpected(make_error(rpc::status::bad_frame, "invalid tensor data size"));
    }

    return {};
}

[[nodiscard]] inline command_result<tensor_request>
decode_tensor_request(std::span<const std::uint8_t> request) {
    auto header_result = decode_tensor_header(request);
    if (!header_result) {
        return std::unexpected(header_result.error());
    }

    const auto& header = header_result.value();
    const std::size_t header_size = get_header_size(header);
    const auto payload = request.subspan(header_size);

    std::size_t payload_size = 0;
    for (const auto& operand : header.operands) {
        payload_size += element_count(operand.shape) * dtype_byte_size(header.dtype);
    }
    if (payload.size() != payload_size) {
        return std::unexpected(make_error(rpc::status::bad_frame, "invalid tensor data size"));
    }

    tensor_request decoded{
        .dtype = header.dtype,
        .domain_type = header.domain_type,
        .output_affine = header.output_affine,
        .operands = {},
    };
    decoded.operands.reserve(header.operands.size());

    std::size_t offset = 0;
    for (std::size_t operand_index = 0; operand_index < header.operands.size(); ++operand_index) {
        const auto& operand = header.operands[operand_index];
        const std::size_t byte_size = element_count(operand.shape) * dtype_byte_size(header.dtype);
        auto desc = tensor_param_desc{
            .dtype = header.dtype,
            .domain_type = header.domain_type,
            .affine = header.domain_type == wire_domain_type::affine
                          ? header.operand_affines[operand_index]
                          : affine_header{},
            .shape = operand.shape,
            .payload = payload.subspan(offset, byte_size),
        };
        if (auto valid = validate_tensor_desc(desc); !valid) {
            return std::unexpected(valid.error());
        }

        decoded.operands.push_back(tensor_request_operand{
            .header = operand,
            .affine = desc.affine,
            .payload = desc.payload,
        });
        offset += byte_size;
    }

    return decoded;
}

template <class Domain, class Shape>
[[nodiscard]] command_result<tensor_param>
build_tensor_param_variant(const tensor_param_desc& desc, std::pmr::memory_resource* mem) {
    return build_tensor_param<Domain, Shape>(desc, mem).transform(
        [](auto param) { return tensor_param{std::move(param)}; });
}

using tensor_builder_fn = command_result<tensor_param> (*)(const tensor_param_desc&,
                                                           std::pmr::memory_resource*);

struct tensor_builder_case {
    wire_domain_type domain_type;
    wire_dtype dtype;
    std::uint8_t rank;
    tensor_builder_fn build;
};

template <class Shape> struct rank_builder {
    static constexpr auto rank = static_cast<std::uint8_t>(Shape::rank_value);

    template <class Domain>
    static constexpr tensor_builder_case make(wire_domain_type domain_type, wire_dtype dtype) {
        return {
            .domain_type = domain_type,
            .dtype = dtype,
            .rank = rank,
            .build = &build_tensor_param_variant<Domain, Shape>,
        };
    }
};

template <class Domain, wire_domain_type DomainType, wire_dtype Dtype>
constexpr auto tensor_type_builder_cases() {
    return std::to_array<tensor_builder_case>({
        rank_builder<tensor_shape<1>>::template make<Domain>(DomainType, Dtype),
        rank_builder<tensor_shape<2>>::template make<Domain>(DomainType, Dtype),
        rank_builder<tensor_shape<3>>::template make<Domain>(DomainType, Dtype),
        rank_builder<tensor_shape<4>>::template make<Domain>(DomainType, Dtype),
    });
}

inline constexpr auto fixed_point_s16_tensor_cases =
    tensor_type_builder_cases<fixed_point_s16, wire_domain_type::fixed_point, wire_dtype::int16>();
inline constexpr auto fixed_point_s8_tensor_cases =
    tensor_type_builder_cases<fixed_point_s8, wire_domain_type::fixed_point, wire_dtype::int8>();
inline constexpr auto affine_s8_tensor_cases =
    tensor_type_builder_cases<affine_s8, wire_domain_type::affine, wire_dtype::int8>();
inline constexpr auto no_quant_tensor_cases =
    tensor_type_builder_cases<no_quant, wire_domain_type::no_quantization, wire_dtype::float32>();

inline constexpr auto tensor_builder_cases = std::to_array<tensor_builder_case>({
    fixed_point_s16_tensor_cases[0],
    fixed_point_s16_tensor_cases[1],
    fixed_point_s16_tensor_cases[2],
    fixed_point_s16_tensor_cases[3],
    fixed_point_s8_tensor_cases[0],
    fixed_point_s8_tensor_cases[1],
    fixed_point_s8_tensor_cases[2],
    fixed_point_s8_tensor_cases[3],
    affine_s8_tensor_cases[0],
    affine_s8_tensor_cases[1],
    affine_s8_tensor_cases[2],
    affine_s8_tensor_cases[3],
    no_quant_tensor_cases[0],
    no_quant_tensor_cases[1],
    no_quant_tensor_cases[2],
    no_quant_tensor_cases[3],
});

[[nodiscard]] inline command_result<tensor_builder_fn>
select_tensor_builder(std::span<const tensor_builder_case> cases, wire_dtype dtype,
                      wire_domain_type domain_type, std::uint8_t rank) {
    if (rank < 1 || rank > 4) {
        return std::unexpected(make_error(rpc::status::bad_frame, "unsupported rank"));
    }
    const auto found = std::ranges::find_if(cases, [&](const tensor_builder_case& item) {
        return item.dtype == dtype && item.domain_type == domain_type && item.rank == rank;
    });

    if (found != cases.end()) {
        return found->build;
    }

    return std::unexpected(make_error(rpc::status::bad_frame, "unsupported tensor type"));
}

[[nodiscard]] inline command_result<tensor_param>
build_tensor_parameter(const tensor_param_desc& desc, std::pmr::memory_resource* mem) {
    return select_tensor_builder(tensor_builder_cases, desc.dtype, desc.domain_type,
                                 static_cast<std::uint8_t>(desc.shape.size()))
        .and_then([&](tensor_builder_fn tensor_from) { return tensor_from(desc, mem); });
}

[[nodiscard]] inline command_result<tensor_parameters>
build_tensor_parameters(const tensor_request& request, std::pmr::memory_resource* mem) {
    tensor_parameters params;
    params.output_affine = request.output_affine;
    params.inputs.reserve(request.operands.size());

    for (const auto& operand : request.operands) {
        auto param = build_tensor_parameter(
            tensor_param_desc{
                .dtype = request.dtype,
                .domain_type = request.domain_type,
                .affine = operand.affine,
                .shape = operand.header.shape,
                .payload = operand.payload,
            },
            mem);
        if (!param) {
            return std::unexpected(param.error());
        }
        params.inputs.push_back(std::move(param.value()));
    }

    return params;
}

} // namespace pbt::tensor_rpc
