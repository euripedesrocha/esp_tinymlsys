# SPDX-FileCopyrightText: 2026
# SPDX-License-Identifier: Unlicense OR CC0-1.0

import tensor_pbt_strategies as strategy
import tensor_rpc


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


def element_count(shape: tuple[int, ...]) -> int:
    count = 1
    for extent in shape:
        count *= extent
    return count


def affine_domains() -> tuple[
    tensor_rpc.AffineDomain, tensor_rpc.AffineDomain, tensor_rpc.AffineDomain
]:
    return (
        tensor_rpc.AffineDomain(scale=1 / 128, zero_point=-7),
        tensor_rpc.AffineDomain(scale=1 / 64, zero_point=5),
        tensor_rpc.AffineDomain(scale=1 / 128, zero_point=-3),
    )


def descriptor(
    tensor_type: tensor_rpc.TensorType,
    shape: tuple[int, ...],
    affine: tensor_rpc.AffineDomain | None = None,
) -> tensor_rpc.TensorDescriptor:
    return tensor_rpc.TensorDescriptor(tensor_type=tensor_type, shape=shape, affine=affine)


def boundary_data(tensor_type: tensor_rpc.TensorType, size: int) -> tuple[list, list]:
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
