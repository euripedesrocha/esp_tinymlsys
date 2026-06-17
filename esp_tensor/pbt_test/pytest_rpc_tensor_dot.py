# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import pytest
from pytest_embedded_idf.utils import idf_parametrize
from hypothesis import given

import tensor_pbt_strategies as strategy
import tensor_references as reference
import tensor_test_helpers as helpers
from rpc import RpcClient
import tensor_rpc


DOT_CASES = [
    tensor_rpc.DOT_FIXED_S8_Q7_TO_S8,
    tensor_rpc.DOT_FIXED_S16_Q15_TO_S16,
    tensor_rpc.DOT_NO_QUANT_F32,
]


def _descriptor(
    tensor_type: tensor_rpc.TensorType,
    shape: tuple[int, ...],
) -> tensor_rpc.TensorDescriptor:
    return tensor_rpc.TensorDescriptor(tensor_type=tensor_type, shape=shape, affine=None)


def _expected_dot(case: strategy.DotCase) -> list:
    tensor_type = case.lhs.tensor_type
    if tensor_type == tensor_rpc.DOT_NO_QUANT_F32:
        return reference.f32_dot(case.lhs_data, case.rhs_data)
    if tensor_type == tensor_rpc.DOT_FIXED_S8_Q7_TO_S8:
        return reference.fixed_point_dot_s8_to_s8(case.lhs_data, case.rhs_data)
    if tensor_type == tensor_rpc.DOT_FIXED_S16_Q15_TO_S16:
        return reference.fixed_point_dot_s16_to_s16(case.lhs_data, case.rhs_data)
    raise AssertionError(f"unsupported dot tensor type: {tensor_type}")


def _assert_matches_reference(
    tensor_type: tensor_rpc.TensorType, actual: list, expected: list
) -> None:
    assert len(actual) == len(expected)
    if tensor_type.dtype == tensor_rpc.Dtype.FLOAT32:
        for a, e in zip(actual, expected):
            assert a == pytest.approx(e, rel=2e-5, abs=1e-3)
    else:
        assert actual == expected


def _case_id(case: strategy.DotCase) -> str:
    return (
        f"dtype={case.lhs.tensor_type.strategy_dtype}, "
        f"output_dtype={case.out.tensor_type.effective_output_dtype}, "
        f"domain={case.lhs.tensor_type.domain_type}, shape={case.lhs.shape}"
    )


def _run_dot_case(rpc_client: RpcClient, case: strategy.DotCase) -> None:
    actual = tensor_rpc.dot(
        rpc_client,
        case.lhs,
        case.rhs,
        case.out,
        lhs=case.lhs_data,
        rhs=case.rhs_data,
    )
    expected = _expected_dot(case)

    try:
        _assert_matches_reference(case.lhs.tensor_type, actual, expected)
    except AssertionError as exc:
        raise AssertionError(_case_id(case)) from exc


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_tensor_dot_properties_match_references(
    rpc_client: RpcClient, request: pytest.FixtureRequest
) -> None:
    max_examples = request.config.getoption("--pbt-examples")

    def run_case(tensor_type: tensor_rpc.TensorType) -> None:
        @strategy.rpc_settings(max_examples=max_examples)
        @given(strategy.dot_case(tensor_type))
        def run(case: strategy.DotCase) -> None:
            _run_dot_case(rpc_client, case)

        run()

    for tensor_type in DOT_CASES:
        run_case(tensor_type)


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_tensor_dot_boundary_shapes_match_references(rpc_client: RpcClient) -> None:
    for tensor_type in DOT_CASES:
        for shape in helpers.BOUNDARY_SHAPES:
            size = helpers.element_count(shape)
            lhs_data, rhs_data = helpers.boundary_data(tensor_type, size)

            case = strategy.DotCase(
                lhs=_descriptor(tensor_type, shape),
                rhs=_descriptor(tensor_type, shape),
                out=_descriptor(tensor_type, (1,)),
                lhs_data=lhs_data,
                rhs_data=rhs_data,
            )

            _run_dot_case(rpc_client, case)


def _dot_frame(
    tensor_type: tensor_rpc.TensorType,
    operand_shapes: list[tuple[int, ...]],
    operand_data: list[list],
    *,
    domain_type: int | None = None,
    output_dtype: int | None = None,
) -> bytes:
    header = tensor_rpc._encode_dot_tensor_header(
        tensor_type.dtype,
        tensor_type.domain_type if domain_type is None else domain_type,
        tensor_type.effective_output_dtype if output_dtype is None else output_dtype,
        [(len(shape), shape) for shape in operand_shapes],
    )
    payload = b"".join(tensor_type.encode(values) for values in operand_data)
    return header + payload


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_tensor_dot_rejects_invalid_rpc_frames(rpc_client: RpcClient) -> None:
    with pytest.raises(RuntimeError, match="dot requires 2 input tensors"):
        rpc_client.call(
            "linear_algebra.dot",
            _dot_frame(tensor_rpc.DOT_FIXED_S8_Q7_TO_S8, [(1,)], [[1]]),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="dot operand shapes must match"):
        rpc_client.call(
            "linear_algebra.dot",
            _dot_frame(
                tensor_rpc.DOT_FIXED_S8_Q7_TO_S8,
                [(1,), (2,)],
                [[1], [1, 2]],
            ),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="unsupported s8 dot output dtype"):
        rpc_client.call(
            "linear_algebra.dot",
            _dot_frame(
                tensor_rpc.DOT_FIXED_S8_Q7_TO_S8,
                [(1,), (1,)],
                [[1], [1]],
                output_dtype=tensor_rpc.Dtype.FLOAT32,
            ),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="float dot output must be float32"):
        rpc_client.call(
            "linear_algebra.dot",
            _dot_frame(
                tensor_rpc.DOT_NO_QUANT_F32,
                [(1,), (1,)],
                [[1.0], [1.0]],
                output_dtype=tensor_rpc.Dtype.INT8,
            ),
        )
    assert rpc_client.ping() == b"pong"

    with pytest.raises(RuntimeError, match="unsupported dot input domain"):
        rpc_client.call(
            "linear_algebra.dot",
            _dot_frame(
                tensor_rpc.DOT_FIXED_S8_Q7_TO_S8,
                [(1,), (1,)],
                [[1], [1]],
                domain_type=tensor_rpc.DomainType.AFFINE,
            ),
        )
    assert rpc_client.ping() == b"pong"
