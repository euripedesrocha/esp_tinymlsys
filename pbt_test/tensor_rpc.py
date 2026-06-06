from __future__ import annotations

import struct
from collections.abc import Callable
from dataclasses import dataclass

from rpc.client import RpcClient


class Dtype:
    FLOAT32 = 0
    INT8 = 1
    INT16 = 2


class DomainType:
    NO_QUANTIZATION = 0
    FIXED_POINT = 1
    AFFINE = 2


@dataclass(frozen=True)
class AffineDomain:
    scale: float = 1.0
    zero_point: int = 0


@dataclass(frozen=True)
class TensorType:
    method: str
    dtype: int
    domain_type: int
    strategy_dtype: str
    encode: Callable[[list], bytes]
    decode: Callable[[bytes], list]


@dataclass(frozen=True)
class TensorDescriptor:
    tensor_type: TensorType
    shape: tuple[int, ...]
    affine: AffineDomain | None = None


def _dtype_size(dtype: int) -> int:
    return {Dtype.FLOAT32: 4, Dtype.INT8: 1, Dtype.INT16: 2}[dtype]


def _element_count(shape: tuple[int, ...]) -> int:
    count = 1
    for extent in shape:
        count *= extent
    return count


def _validate_add_operands(
    lhs_desc: TensorDescriptor,
    rhs_desc: TensorDescriptor,
    out_desc: TensorDescriptor,
    lhs: list,
    rhs: list,
    lhs_payload: bytes,
    rhs_payload: bytes,
) -> None:
    if (
        lhs_desc.tensor_type != rhs_desc.tensor_type
        or lhs_desc.tensor_type != out_desc.tensor_type
    ):
        raise ValueError("add operands must use the same tensor type")
    if lhs_desc.shape != rhs_desc.shape or lhs_desc.shape != out_desc.shape:
        raise ValueError("add operands must use the same shape")

    expected_elements = _element_count(out_desc.shape)
    if len(lhs) != expected_elements or len(rhs) != expected_elements:
        raise ValueError(
            f"add payload length mismatch: expected {expected_elements}, "
            f"got lhs={len(lhs)}, rhs={len(rhs)}"
        )
    expected_bytes = expected_elements * _dtype_size(out_desc.tensor_type.dtype)
    if len(lhs_payload) != expected_bytes or len(rhs_payload) != expected_bytes:
        raise ValueError(
            f"encoded add payload size mismatch: expected {expected_bytes}, "
            f"got lhs={len(lhs_payload)}, rhs={len(rhs_payload)}"
        )


def _encode_shape(shape: tuple[int, ...]) -> bytes:
    return b"".join(struct.pack("<I", dim) for dim in shape)


def _encode_affine_domain(domain: AffineDomain) -> bytes:
    return struct.pack("<fi", domain.scale, domain.zero_point)


def _encode_tensor_header(
    dtype: int,
    domain_type: int,
    operand_shapes: list[tuple[int, tuple[int, ...]]],
    *,
    output_domain: AffineDomain | None = None,
    operand_domains: list[AffineDomain | None] | None = None,
) -> bytes:
    header = struct.pack("<BB", dtype, domain_type)
    header += struct.pack("<B", len(operand_shapes))
    if domain_type == DomainType.AFFINE:
        output_domain = output_domain or AffineDomain()
        operand_domains = operand_domains or []
        operand_domains = [
            operand_domains[index]
            if index < len(operand_domains) and operand_domains[index] is not None
            else AffineDomain()
            for index in range(len(operand_shapes))
        ]
        header += _encode_affine_domain(output_domain)
    for index, (rank, shape) in enumerate(operand_shapes):
        header += struct.pack("<B", rank)
        header += _encode_shape(shape)
        if domain_type == DomainType.AFFINE:
            header += _encode_affine_domain(operand_domains[index])
    return header


def _encode_s16_le(values: list[int]) -> bytes:
    return b"".join(struct.pack("<h", value) for value in values)


def _decode_s16_le(payload: bytes) -> list[int]:
    return [value[0] for value in struct.iter_unpack("<h", payload)]


def _encode_s8(values: list[int]) -> bytes:
    return bytes(value & 0xFF for value in values)


def _decode_s8(payload: bytes) -> list[int]:
    return [b if b < 128 else b - 256 for b in payload]


def _encode_f32_le(values: list[float]) -> bytes:
    return b"".join(struct.pack("<f", value) for value in values)


def _decode_f32_le(payload: bytes) -> list[float]:
    return [value[0] for value in struct.iter_unpack("<f", payload)]


FIXED_S16_Q15 = TensorType(
    method="arith.add",
    dtype=Dtype.INT16,
    domain_type=DomainType.FIXED_POINT,
    strategy_dtype="s16",
    encode=_encode_s16_le,
    decode=_decode_s16_le,
)

FIXED_S8_Q7 = TensorType(
    method="arith.add",
    dtype=Dtype.INT8,
    domain_type=DomainType.FIXED_POINT,
    strategy_dtype="s8",
    encode=_encode_s8,
    decode=_decode_s8,
)

AFFINE_S8 = TensorType(
    method="arith.add",
    dtype=Dtype.INT8,
    domain_type=DomainType.AFFINE,
    strategy_dtype="s8",
    encode=_encode_s8,
    decode=_decode_s8,
)

NO_QUANT_F32 = TensorType(
    method="arith.add",
    dtype=Dtype.FLOAT32,
    domain_type=DomainType.NO_QUANTIZATION,
    strategy_dtype="f32",
    encode=_encode_f32_le,
    decode=_decode_f32_le,
)


def add(
    client: RpcClient,
    lhs_desc: TensorDescriptor,
    rhs_desc: TensorDescriptor,
    out_desc: TensorDescriptor,
    lhs: list,
    rhs: list,
) -> list:
    lhs_payload = lhs_desc.tensor_type.encode(lhs)
    rhs_payload = rhs_desc.tensor_type.encode(rhs)
    _validate_add_operands(
        lhs_desc, rhs_desc, out_desc, lhs, rhs, lhs_payload, rhs_payload
    )

    operand_domains = None
    # Preserve the compact fixed-point/no-quantization frame. Affine metadata is
    # appended only for affine tensor domains.
    if out_desc.tensor_type.domain_type == DomainType.AFFINE:
        operand_domains = [lhs_desc.affine, rhs_desc.affine]

    header = _encode_tensor_header(
        out_desc.tensor_type.dtype,
        out_desc.tensor_type.domain_type,
        operand_shapes=[
            (len(lhs_desc.shape), lhs_desc.shape),
            (len(rhs_desc.shape), rhs_desc.shape),
        ],
        output_domain=out_desc.affine,
        operand_domains=operand_domains,
    )
    response = client.call(
        out_desc.tensor_type.method,
        header + lhs_payload + rhs_payload,
    )
    return out_desc.tensor_type.decode(response)
