"""WS63 route-id derivation shared by serial test tools."""

from __future__ import annotations

ROUTE_ID_SUFFIX_HIGH_WEIGHT = 31


def route_id_from_suffix(suffix: int) -> int:
    value = suffix & 0xFFFF
    low = value & 0xFF
    high = (value >> 8) & 0xFF
    mix = low + (high * ROUTE_ID_SUFFIX_HIGH_WEIGHT)
    return (mix % 254) + 1
