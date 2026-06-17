from __future__ import annotations

import math
import struct

from tensor_rpc import AffineDomain


def _bounds(dtype: str) -> tuple[int, int]:
    match dtype:
        case "s8":
            return -128, 127
        case "s16":
            return -32768, 32767
        case "s32":
            return -(2**31), (2**31) - 1
        case _:
            raise ValueError(f"unsupported integer dtype: {dtype}")


def saturating_add(lhs: list[int], rhs: list[int], *, dtype: str) -> list[int]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")
    min_value, max_value = _bounds(dtype)
    return [max(min_value, min(max_value, a + b)) for a, b in zip(lhs, rhs)]


def _to_i32(value: int) -> int:
    value &= 0xFFFFFFFF
    return value - 0x100000000 if value & 0x80000000 else value


def _cpp_llround(value: float) -> int:
    return math.floor(value + 0.5) if value >= 0.0 else math.ceil(value - 0.5)


def _make_quantized_multiplier(scale: float) -> tuple[int, int]:
    if scale == 0.0 or not math.isfinite(scale):
        return 0, 0

    significand, exponent = math.frexp(scale)
    multiplier = _cpp_llround(significand * float(1 << 31))
    if multiplier == (1 << 31):
        multiplier //= 2
        exponent += 1
    return _to_i32(multiplier), exponent


def _affine_scale_ratio(input_domain: AffineDomain, output_domain: AffineDomain) -> float:
    return _f32(input_domain.scale) / _f32(output_domain.scale)


def _esp_nn_fast_round_doubling_high_mul(lhs: int, rhs: int) -> int:
    lhs = _to_i32(lhs)
    rhs = _to_i32(rhs)
    product = lhs * rhs + (1 << 30)
    return _to_i32(product >> 31)


def _esp_nn_fast_div_by_power_of_two(value: int, exponent: int) -> int:
    value = _to_i32(value)
    if exponent <= 0:
        return value
    return _to_i32((value + ((1 << (exponent - 1)) - (1 if value < 0 else 0))) >> exponent)


def _esp_nn_add_s8_input(value: int, multiplier: tuple[int, int], left_shift: int) -> int:
    integer_multiplier, shift = multiplier
    shifted = _to_i32(value << left_shift)
    multiplied = _esp_nn_fast_round_doubling_high_mul(shifted, integer_multiplier)
    return _esp_nn_fast_div_by_power_of_two(multiplied, -(shift - left_shift))


def affine_add_s8_esp_nn(
    lhs: list[int],
    rhs: list[int],
    *,
    lhs_domain: AffineDomain,
    rhs_domain: AffineDomain,
    output_domain: AffineDomain,
) -> list[int]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")

    lhs_multiplier = _make_quantized_multiplier(_affine_scale_ratio(lhs_domain, output_domain))
    rhs_multiplier = _make_quantized_multiplier(_affine_scale_ratio(rhs_domain, output_domain))
    left_shift = max(0, lhs_multiplier[1], rhs_multiplier[1])

    result = []
    for lhs_value, rhs_value in zip(lhs, rhs):
        lhs_term = _esp_nn_add_s8_input(
            lhs_value - lhs_domain.zero_point, lhs_multiplier, left_shift
        )
        rhs_term = _esp_nn_add_s8_input(
            rhs_value - rhs_domain.zero_point, rhs_multiplier, left_shift
        )
        out = _esp_nn_fast_round_doubling_high_mul(
            _to_i32(lhs_term + rhs_term), (2**31) - 1
        )
        result.append(max(-128, min(127, out + output_domain.zero_point)))
    return result

def _f32(value: float) -> float:
    return struct.unpack("<f", struct.pack("<f", value))[0]


def f32_add(lhs: list[float], rhs: list[float]) -> list[float]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")
    return [_f32(_f32(a) + _f32(b)) for a, b in zip(lhs, rhs)]


def f32_dot(lhs: list[float], rhs: list[float]) -> list[float]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")
    total = 0.0
    for a, b in zip(lhs, rhs):
        total = _f32(total + _f32(_f32(a) * _f32(b)))
    return [_f32(total)]


def _saturating_cast(value: int, min_value: int, max_value: int) -> int:
    return max(min_value, min(max_value, value))


def _scale_raw_fixed_point_dot(value: int, input_bits: int, output_bits: int,
                               output_min: int, output_max: int) -> int:
    shift = 2 * input_bits - output_bits
    if shift > 0:
        rounded = (value + (1 << (shift - 1))) >> shift
        return _saturating_cast(rounded, output_min, output_max)
    if shift < 0:
        left_shift = -shift
        lower = output_min >> left_shift
        upper = output_max >> left_shift
        if value < lower:
            return output_min
        if value > upper:
            return output_max
        return value << left_shift
    return _saturating_cast(value, output_min, output_max)


def fixed_point_dot_s8_to_s8(lhs: list[int], rhs: list[int]) -> list[int]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")
    total = sum(a * b for a, b in zip(lhs, rhs))
    return [_scale_raw_fixed_point_dot(total, 7, 7, -128, 127)]


def fixed_point_dot_s16_to_s16(lhs: list[int], rhs: list[int]) -> list[int]:
    if len(lhs) != len(rhs):
        raise ValueError("lhs and rhs must have the same length")
    total = sum(a * b for a, b in zip(lhs, rhs))
    return [_scale_raw_fixed_point_dot(total, 15, 15, -32768, 32767)]
