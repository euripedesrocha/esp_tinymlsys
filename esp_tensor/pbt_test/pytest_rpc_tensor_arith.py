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


ADD_CASES = [
    tensor_rpc.FIXED_S16_Q15,
    tensor_rpc.FIXED_S8_Q7,
    tensor_rpc.AFFINE_S8,
    tensor_rpc.NO_QUANT_F32,
]

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
def test_tensor_add_properties_match_references(
    rpc_client: RpcClient, request: pytest.FixtureRequest
) -> None:
    max_examples = request.config.getoption("--pbt-examples")

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
def test_tensor_add_boundary_shapes_match_references(rpc_client: RpcClient) -> None:
    for tensor_type in ADD_CASES:
        for shape in helpers.BOUNDARY_SHAPES:
            size = helpers.element_count(shape)
            lhs_data, rhs_data = helpers.boundary_data(tensor_type, size)

            lhs_affine = rhs_affine = out_affine = None
            if tensor_type.domain_type == tensor_rpc.DomainType.AFFINE:
                lhs_affine, rhs_affine, out_affine = helpers.affine_domains()

            case = strategy.BinaryElementwiseCase(
                lhs=helpers.descriptor(tensor_type, shape, lhs_affine),
                rhs=helpers.descriptor(tensor_type, shape, rhs_affine),
                out=helpers.descriptor(tensor_type, shape, out_affine),
                lhs_data=lhs_data,
                rhs_data=rhs_data,
            )

            _run_add_case(rpc_client, case)
