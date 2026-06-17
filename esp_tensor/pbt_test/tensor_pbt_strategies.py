from __future__ import annotations

from dataclasses import dataclass
from functools import reduce
import math
from operator import mul

from hypothesis import HealthCheck, settings
from hypothesis import strategies as st

from tensor_rpc import AffineDomain, DomainType, TensorDescriptor, TensorType

S8 = "s8"
S16 = "s16"
F32 = "f32"


@dataclass(frozen=True)
class BinaryElementwiseCase:
    lhs: TensorDescriptor
    rhs: TensorDescriptor
    out: TensorDescriptor
    lhs_data: list[int] | list[float]
    rhs_data: list[int] | list[float]


@dataclass(frozen=True)
class DotCase:
    lhs: TensorDescriptor
    rhs: TensorDescriptor
    out: TensorDescriptor
    lhs_data: list[int] | list[float]
    rhs_data: list[int] | list[float]


def rpc_settings(max_examples: int):
    return settings(
        deadline=None,
        max_examples=max_examples,
        suppress_health_check=[HealthCheck.function_scoped_fixture],
    )


def shape(
    *, min_rank: int = 1, max_rank: int = 4, max_dim: int = 16, max_elements: int = 1024
) -> st.SearchStrategy[tuple[int, ...]]:
    edge_shapes = [
        (0,),
        (1,),
        (7,),
        (8,),
        (9,),
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
        (0, 3),
        (3, 0),
        (2, 3),
        (4, 4),
        (8, 8),
        (16, 16),
        (2, 3, 4),
        (4, 4, 4),
        (8, 8, 2),
        (1, 2, 3, 4),
        (4, 4, 4, 4),
    ]

    @st.composite
    def draw_shape(draw: st.DrawFn) -> tuple[int, ...]:
        rank = draw(st.integers(min_value=min_rank, max_value=max_rank))
        return draw(
            st.tuples(
                *(st.integers(min_value=0, max_value=max_dim) for _ in range(rank))
            )
        )

    return st.one_of(
        st.sampled_from(
            [
                shape
                for shape in edge_shapes
                if min_rank <= len(shape) <= max_rank
                and reduce(mul, shape, 1) <= max_elements
            ]
        ),
        draw_shape().filter(lambda shape: reduce(mul, shape, 1) <= max_elements),
    )


def _element_count(shape: tuple[int, ...]) -> int:
    return reduce(mul, shape, 1)


def _integer_element(min_value: int, max_value: int) -> st.SearchStrategy[int]:
    edge_values = [min_value, min_value + 1, -1, 0, 1, max_value - 1, max_value]
    return st.one_of(
        st.sampled_from(
            [value for value in edge_values if min_value <= value <= max_value]
        ),
        st.integers(min_value=min_value, max_value=max_value),
    )


def _float_element() -> st.SearchStrategy[float]:
    edge_values = [-1e6, -1.0, -0.0, 0.0, 1.0, 1e-6, 1e6]
    return st.one_of(
        st.sampled_from(edge_values),
        st.floats(min_value=-1e6, max_value=1e6, allow_nan=False, allow_infinity=False),
    )


def _dot_float_element() -> st.SearchStrategy[float]:
    edge_values = [-16.0, -1.0, -0.0, 0.0, 1e-3, 1.0, 16.0]
    return st.one_of(
        st.sampled_from(edge_values),
        st.floats(
            min_value=-16.0,
            max_value=16.0,
            allow_nan=False,
            allow_infinity=False,
            width=32,
        ),
    )


def _bounds(dtype: str) -> tuple[int, int]:
    match dtype:
        case "s8":
            return -128, 127
        case "s16":
            return -32768, 32767
        case _:
            raise ValueError(f"unsupported integer dtype: {dtype}")


def vector(
    shape: tuple[int, ...], *, dtype: str
) -> st.SearchStrategy[list[int] | list[float]]:
    size = _element_count(shape)
    if dtype == F32:
        element = _float_element()
    else:
        min_value, max_value = _bounds(dtype)
        element = _integer_element(min_value, max_value)
    return st.lists(element, min_size=size, max_size=size)


def dot_vector(
    shape: tuple[int, ...], *, dtype: str
) -> st.SearchStrategy[list[int] | list[float]]:
    size = _element_count(shape)
    if dtype == F32:
        return st.lists(_dot_float_element(), min_size=size, max_size=size)
    return vector(shape, dtype=dtype)


def _realistic_affine_scale() -> st.SearchStrategy[float]:
    return st.one_of(
        st.sampled_from([1 / 256, 1 / 128, 1 / 64, 1 / 32, 0.05, 0.1, 0.2, 0.5]),
        st.floats(
            min_value=1 / 1024,
            max_value=0.5,
            allow_nan=False,
            allow_infinity=False,
            allow_subnormal=False,
            width=32,
        ),
    )


def _realistic_affine_scale_ratio() -> st.SearchStrategy[float]:
    return st.one_of(
        st.sampled_from([0.25, 0.5, 1.0, 2.0, 4.0]),
        st.floats(
            min_value=0.25,
            max_value=4.0,
            allow_nan=False,
            allow_infinity=False,
            allow_subnormal=False,
            width=32,
        ),
    )


def _valid_model_scale(scale: float) -> bool:
    return math.isfinite(scale) and 1 / 16384 <= scale <= 2.0


def _affine_zero_point(dtype: str, *, operand: bool) -> st.SearchStrategy[int]:
    min_value, max_value = _bounds(dtype)
    if dtype == S8 and operand:
        min_value = -127

    zero_points = st.one_of(
        st.sampled_from(
            [value for value in [min_value, -127, -1, 0, 1, 126, max_value]
             if min_value <= value <= max_value]
        ),
        st.integers(min_value=min_value, max_value=max_value),
    )
    return zero_points


def affine_domain_triplet(tensor_type: TensorType) -> st.SearchStrategy[
    tuple[AffineDomain, AffineDomain, AffineDomain]
]:
    @st.composite
    def draw_triplet(draw: st.DrawFn) -> tuple[AffineDomain, AffineDomain, AffineDomain]:
        dtype = tensor_type.strategy_dtype
        output_scale = draw(_realistic_affine_scale(), label="output_scale")
        lhs_scale = output_scale * draw(
            _realistic_affine_scale_ratio(), label="lhs_scale_ratio"
        )
        rhs_scale = output_scale * draw(
            _realistic_affine_scale_ratio(), label="rhs_scale_ratio"
        )
        assert _valid_model_scale(lhs_scale)
        assert _valid_model_scale(rhs_scale)

        return (
            AffineDomain(
                scale=lhs_scale,
                zero_point=draw(_affine_zero_point(dtype, operand=True), label="lhs_zp"),
            ),
            AffineDomain(
                scale=rhs_scale,
                zero_point=draw(_affine_zero_point(dtype, operand=True), label="rhs_zp"),
            ),
            AffineDomain(
                scale=output_scale,
                zero_point=draw(_affine_zero_point(dtype, operand=False), label="output_zp"),
            ),
        )

    return draw_triplet()


def _valid_binary_elementwise_case(case: BinaryElementwiseCase) -> bool:
    same_tensor_type = (
        case.lhs.tensor_type == case.rhs.tensor_type == case.out.tensor_type
    )
    same_shape = case.lhs.shape == case.rhs.shape == case.out.shape
    same_payload_size = (
        len(case.lhs_data) == len(case.rhs_data) == _element_count(case.out.shape)
    )
    affine_has_domains = (
        case.lhs.affine is not None
        and case.rhs.affine is not None
        and case.out.affine is not None
    )
    non_affine_has_no_domains = (
        case.lhs.affine is None and case.rhs.affine is None and case.out.affine is None
    )
    domains_match_tensor_type = (
        affine_has_domains
        if case.out.tensor_type.domain_type == DomainType.AFFINE
        else non_affine_has_no_domains
    )
    return same_tensor_type and same_shape and same_payload_size and domains_match_tensor_type


def _tensor_descriptor(
    tensor_type: TensorType, tensor_shape: tuple[int, ...], affine: AffineDomain | None
) -> TensorDescriptor:
    return TensorDescriptor(tensor_type=tensor_type, shape=tensor_shape, affine=affine)


def binary_elementwise_case(tensor_type: TensorType) -> st.SearchStrategy[BinaryElementwiseCase]:
    @st.composite
    def draw_case(draw: st.DrawFn) -> BinaryElementwiseCase:
        tensor_shape = draw(shape())
        lhs_domain = rhs_domain = output_domain = None
        if tensor_type.domain_type == DomainType.AFFINE:
            lhs_domain, rhs_domain, output_domain = draw(
                affine_domain_triplet(tensor_type), label="affine_domains"
            )
        case = BinaryElementwiseCase(
            lhs=_tensor_descriptor(tensor_type, tensor_shape, lhs_domain),
            rhs=_tensor_descriptor(tensor_type, tensor_shape, rhs_domain),
            out=_tensor_descriptor(tensor_type, tensor_shape, output_domain),
            lhs_data=draw(
                vector(tensor_shape, dtype=tensor_type.strategy_dtype), label="lhs"
            ),
            rhs_data=draw(
                vector(tensor_shape, dtype=tensor_type.strategy_dtype), label="rhs"
            ),
        )
        assert _valid_binary_elementwise_case(case)
        return case

    return draw_case()


def _valid_dot_case(case: DotCase) -> bool:
    same_input_type = case.lhs.tensor_type == case.rhs.tensor_type
    same_input_shape = case.lhs.shape == case.rhs.shape
    scalar_output = case.out.shape == (1,)
    same_payload_size = (
        len(case.lhs_data) == len(case.rhs_data) == _element_count(case.lhs.shape)
    )
    no_affine = (
        case.lhs.affine is None
        and case.rhs.affine is None
        and case.out.affine is None
    )
    return (
        same_input_type
        and same_input_shape
        and scalar_output
        and same_payload_size
        and no_affine
    )


def dot_case(tensor_type: TensorType) -> st.SearchStrategy[DotCase]:
    @st.composite
    def draw_case(draw: st.DrawFn) -> DotCase:
        input_shape = draw(shape())
        case = DotCase(
            lhs=_tensor_descriptor(tensor_type, input_shape, None),
            rhs=_tensor_descriptor(tensor_type, input_shape, None),
            out=_tensor_descriptor(tensor_type, (1,), None),
            lhs_data=draw(dot_vector(input_shape, dtype=tensor_type.strategy_dtype), label="lhs"),
            rhs_data=draw(dot_vector(input_shape, dtype=tensor_type.strategy_dtype), label="rhs"),
        )
        assert _valid_dot_case(case)
        return case

    return draw_case()
