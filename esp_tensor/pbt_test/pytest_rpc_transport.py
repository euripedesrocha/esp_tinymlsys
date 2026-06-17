# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import pytest
from pytest_embedded_idf.utils import idf_parametrize

from rpc import RpcClient
import tensor_rpc


def _encode_shape(shape: tuple[int, ...]) -> bytes:
    return b"".join(dim.to_bytes(4, "little") for dim in shape)


def _encode_tensor_header(
    dtype: int,
    domain_type: int,
    operand_shapes: list[tuple[int, tuple[int, ...]]],
) -> bytes:
    header = bytes([dtype, domain_type])
    header += bytes([len(operand_shapes)])
    for rank, shape in operand_shapes:
        header += bytes([rank])
        header += _encode_shape(shape)
    return header


def _encode_s16_le(values: list[int]) -> bytes:
    return b"".join(value.to_bytes(2, "little", signed=True) for value in values)


def _descriptor(tensor_type: tensor_rpc.TensorType, shape: tuple[int, ...]) -> tensor_rpc.TensorDescriptor:
    return tensor_rpc.TensorDescriptor(tensor_type=tensor_type, shape=shape)


@pytest.mark.supported_targets
@pytest.mark.generic
@pytest.mark.rpc_transport
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_rpc_transport_smoke(rpc_client: RpcClient) -> None:
    assert rpc_client.ping() == b"pong"
    result = tensor_rpc.add(
        rpc_client,
        _descriptor(tensor_rpc.FIXED_S16_Q15, (3,)),
        _descriptor(tensor_rpc.FIXED_S16_Q15, (3,)),
        _descriptor(tensor_rpc.FIXED_S16_Q15, (3,)),
        lhs=[16384, -16384, 32767],
        rhs=[8192, -8192, 1],
    )
    assert result == [24576, -24576, 32767]

    result = tensor_rpc.add(
        rpc_client,
        _descriptor(tensor_rpc.FIXED_S16_Q15, (1,)),
        _descriptor(tensor_rpc.FIXED_S16_Q15, (1,)),
        _descriptor(tensor_rpc.FIXED_S16_Q15, (1,)),
        lhs=[0],
        rhs=[0],
    )
    assert result == [0]


@pytest.mark.supported_targets
@pytest.mark.generic
@pytest.mark.rpc_transport
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_rpc_large_payload_sequential(rpc_client: RpcClient) -> None:
    assert rpc_client.ping() == b"pong"

    count = 50
    size = 256
    values = [i % 32768 for i in range(size)]
    expected = [
        (2 * value) if abs(2 * value) <= 32767 else 32767 for value in values
    ]

    for i in range(count):
        actual = tensor_rpc.add(
            rpc_client,
            _descriptor(tensor_rpc.FIXED_S16_Q15, (size,)),
            _descriptor(tensor_rpc.FIXED_S16_Q15, (size,)),
            _descriptor(tensor_rpc.FIXED_S16_Q15, (size,)),
            lhs=values,
            rhs=values,
        )
        assert actual == expected, f"iteration {i} mismatch"


@pytest.mark.supported_targets
@pytest.mark.generic
@pytest.mark.rpc_transport
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_rpc_reports_method_and_tensor_frame_errors(rpc_client: RpcClient) -> None:
    with pytest.raises(RuntimeError, match="UNKNOWN_METHOD"):
        rpc_client.call("rpc.missing", b"")
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="invalid tensor data size"):
        rpc_client.call(
            "arith.add",
            _encode_tensor_header(
                tensor_rpc.Dtype.FLOAT32,
                tensor_rpc.DomainType.FIXED_POINT,
                [(1, (1,))],
            ),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="invalid tensor data size"):
        rpc_client.call(
            "arith.add",
            _encode_tensor_header(
                tensor_rpc.Dtype.INT16,
                tensor_rpc.DomainType.NO_QUANTIZATION,
                [(1, (1,))],
            ),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="unsupported rank"):
        rpc_client.call(
            "arith.add",
            _encode_tensor_header(
                tensor_rpc.Dtype.INT16,
                tensor_rpc.DomainType.FIXED_POINT,
                [(5, (1, 1, 1, 1, 1)), (5, (1, 1, 1, 1, 1))],
            )
            + _encode_s16_le([0])
            + _encode_s16_le([0]),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="invalid tensor data size"):
        rpc_client.call(
            "arith.add",
            _encode_tensor_header(
                tensor_rpc.Dtype.INT16,
                tensor_rpc.DomainType.FIXED_POINT,
                [(1, (2,)), (1, (2,))],
            )
            + _encode_s16_le([0])
            + _encode_s16_le([0]),
        )
    assert rpc_client.ping() == b"pong"


@pytest.mark.supported_targets
@pytest.mark.generic
@pytest.mark.rpc_transport
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_rpc_client_reopens_without_physical_reset(dut) -> None:
    for _ in range(3):
        with RpcClient(dut.serial) as rpc_client:
            assert rpc_client.ping() == b"pong"
