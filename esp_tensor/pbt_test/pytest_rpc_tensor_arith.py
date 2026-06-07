# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import pytest
from pytest_embedded_idf.utils import idf_parametrize
from hypothesis import given

import tensor_pbt_strategies as strategy
import tensor_references as reference
from rpc import RpcClient
import tensor_rpc


ADD_CASES = [
    tensor_rpc.FIXED_S16_Q15,
    tensor_rpc.FIXED_S8_Q7,
    tensor_rpc.AFFINE_S8,
    tensor_rpc.NO_QUANT_F32,
]

BOUNDARY_SHAPES = [
    (0,),
    (1,),
    (15,),
    (16,),
    (17,),
    (31,),
    (32,),
    (33,),
    (63,),
    (64,),
    (65,),
    (127,),
    (128,),
    (129,),
    (255,),
    (256,),
    (257,),
    (511,),
    (512,),
    (513,),
    (1023,),
    (1024,),
    (4, 4),
    (8, 8),
    (16, 16),
    (4, 4, 4),
    (8, 8, 2),
    (4, 4, 4, 4),
]


def _element_count(shape: tuple[int, ...]) -> int:
    count = 1
    for extent in shape:
        count *= extent
    return count


def _affine_domains() -> tuple[
    tensor_rpc.AffineDomain, tensor_rpc.AffineDomain, tensor_rpc.AffineDomain
]:
    return (
        tensor_rpc.AffineDomain(scale=1 / 128, zero_point=-7),
        tensor_rpc.AffineDomain(scale=1 / 64, zero_point=5),
        tensor_rpc.AffineDomain(scale=1 / 128, zero_point=-3),
    )


def _descriptor(
    tensor_type: tensor_rpc.TensorType,
    shape: tuple[int, ...],
    affine: tensor_rpc.AffineDomain | None = None,
) -> tensor_rpc.TensorDescriptor:
    return tensor_rpc.TensorDescriptor(tensor_type=tensor_type, shape=shape, affine=affine)


def _boundary_data(tensor_type: tensor_rpc.TensorType, size: int) -> tuple[list, list]:
    if tensor_type.strategy_dtype == strategy.F32:
        lhs_pattern = [-1e6, -1.0, -0.0, 0.0, 1e-6, 1.0, 1e6]
        rhs_pattern = [1e6, 1.0, 0.0, -0.0, -1e-6, -1.0, -1e6]
    elif tensor_type.strategy_dtype == strategy.S16:
        lhs_pattern = [-32768, -32767, -1, 0, 1, 32766, 32767]
        rhs_pattern = [-1, 1, -32768, 0, 32767, 1, 1]
    elif tensor_type.strategy_dtype == strategy.S8:
        lhs_pattern = [-128, -127, -1, 0, 1, 126, 127]
        rhs_pattern = [-1, 1, -128, 0, 127, 1, 1]
    else:
        raise AssertionError(f"unsupported strategy dtype: {tensor_type.strategy_dtype}")

    lhs = [lhs_pattern[index % len(lhs_pattern)] for index in range(size)]
    rhs = [rhs_pattern[index % len(rhs_pattern)] for index in range(size)]
    return lhs, rhs


def _expected_add(case: strategy.BinaryElementwiseCase) -> list:
    tensor_type = case.out.tensor_type
    if tensor_type.domain_type == tensor_rpc.DomainType.AFFINE:
        assert case.lhs.affine is not None
        assert case.rhs.affine is not None
        assert case.out.affine is not None
        if tensor_type == tensor_rpc.AFFINE_S8:
            return reference.affine_add_s8_esp_nn(
                case.lhs_data,
                case.rhs_data,
                lhs_domain=case.lhs.affine,
                rhs_domain=case.rhs.affine,
                output_domain=case.out.affine,
            )
        raise AssertionError(f"unsupported affine add tensor type: {tensor_type}")
    if tensor_type.domain_type == tensor_rpc.DomainType.FIXED_POINT:
        return reference.saturating_add(
            case.lhs_data, case.rhs_data, dtype=tensor_type.strategy_dtype
        )
    return reference.f32_add(case.lhs_data, case.rhs_data)


def _assert_matches_reference(
    tensor_type: tensor_rpc.TensorType, actual: list, expected: list
) -> None:
    assert len(actual) == len(expected)
    if tensor_type.dtype == tensor_rpc.Dtype.FLOAT32:
        for a, e in zip(actual, expected):
            assert a == pytest.approx(e, rel=1e-5)
    else:
        assert actual == expected


def _case_id(case: strategy.BinaryElementwiseCase) -> str:
    return (
        f"dtype={case.out.tensor_type.strategy_dtype}, "
        f"domain={case.out.tensor_type.domain_type}, shape={case.out.shape}"
    )


def _run_add_case(rpc_client: RpcClient, case: strategy.BinaryElementwiseCase) -> None:
    actual = tensor_rpc.add(
        rpc_client,
        case.lhs,
        case.rhs,
        case.out,
        lhs=case.lhs_data,
        rhs=case.rhs_data,
    )
    expected = _expected_add(case)

    try:
        _assert_matches_reference(case.out.tensor_type, actual, expected)
    except AssertionError as exc:
        raise AssertionError(_case_id(case)) from exc


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_tensor_add_properties_match_references(dut, request: pytest.FixtureRequest) -> None:
    max_examples = request.config.getoption("--pbt-examples")

    with RpcClient(dut.serial) as rpc_client:

        def run_case(tensor_type: tensor_rpc.TensorType) -> None:
            @strategy.rpc_settings(max_examples=max_examples)
            @given(strategy.binary_elementwise_case(tensor_type))
            def run(case: strategy.BinaryElementwiseCase) -> None:
                _run_add_case(rpc_client, case)

            run()

        for tensor_type in ADD_CASES:
            run_case(tensor_type)


@pytest.mark.supported_targets
@pytest.mark.generic
@idf_parametrize("target", ["esp32s3"], indirect=["target"])
def test_tensor_add_boundary_shapes_match_references(dut) -> None:
    with RpcClient(dut.serial) as rpc_client:
        for tensor_type in ADD_CASES:
            for shape in BOUNDARY_SHAPES:
                size = _element_count(shape)
                lhs_data, rhs_data = _boundary_data(tensor_type, size)

                lhs_affine = rhs_affine = out_affine = None
                if tensor_type.domain_type == tensor_rpc.DomainType.AFFINE:
                    lhs_affine, rhs_affine, out_affine = _affine_domains()

                case = strategy.BinaryElementwiseCase(
                    lhs=_descriptor(tensor_type, shape, lhs_affine),
                    rhs=_descriptor(tensor_type, shape, rhs_affine),
                    out=_descriptor(tensor_type, shape, out_affine),
                    lhs_data=lhs_data,
                    rhs_data=rhs_data,
                )

                _run_add_case(rpc_client, case)
