#!/usr/bin/env python3
"""WS63 serial-port preflight checks for live hardware tests."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from typing import Iterable, Sequence

try:
    from serial.tools import list_ports  # type: ignore
except ImportError as exc:  # pragma: no cover
    raise SystemExit("pyserial is required: pip install pyserial") from exc


DEFAULT_EXCLUDE = {"COM1"}
BOARD_KEYWORDS = ("CH340", "USB-SERIAL", "JLink CDC UART", "CP210", "USB VID:PID=1A86:7523")


@dataclass(frozen=True)
class PortInfo:
    device: str
    description: str
    hwid: str


def list_serial_ports() -> list[PortInfo]:
    ports: list[PortInfo] = []
    for item in list_ports.comports():
        ports.append(PortInfo(device=item.device, description=item.description or "", hwid=item.hwid or ""))
    return ports


def _parse_csv(value: str) -> list[str]:
    return [item.strip() for item in value.split(",") if item.strip()]


def is_board_port(port: PortInfo, exclude: Iterable[str] = DEFAULT_EXCLUDE) -> bool:
    excluded = {item.upper() for item in exclude}
    if port.device.upper() in excluded:
        return False
    haystack = f"{port.device} {port.description} {port.hwid}".upper()
    return any(keyword.upper() in haystack for keyword in BOARD_KEYWORDS)


def select_board_ports(ports: Sequence[PortInfo], exclude: Iterable[str] = DEFAULT_EXCLUDE) -> list[PortInfo]:
    return [port for port in ports if is_board_port(port, exclude)]


def validate_requested_ports(available: Sequence[PortInfo], requested: Sequence[str]) -> tuple[bool, list[str]]:
    available_by_name = {port.device.upper(): port for port in available}
    errors: list[str] = []
    seen: set[str] = set()

    if len(requested) < 3:
        errors.append("relay-cycle requires three ports: leader,relay,child")
    for port_name in requested:
        key = port_name.upper()
        if key in seen:
            errors.append(f"duplicate port: {port_name}")
            continue
        seen.add(key)
        port = available_by_name.get(key)
        if port is None:
            errors.append(f"port not available: {port_name}")
        elif not is_board_port(port):
            errors.append(f"port does not look like a WS63 board UART: {port_name}")
    return len(errors) == 0, errors


def build_relay_cycle_command(ports: Sequence[str]) -> str:
    return "bash automation/ws63/scripts/ws63_test_system.sh --with-relay-cycle --ports " + ",".join(ports[:3])


def run(args: argparse.Namespace) -> int:
    available = list_serial_ports()
    board_ports = select_board_ports(available, exclude=_parse_csv(args.exclude))
    requested = _parse_csv(args.ports)

    print("[serial-preflight] available ports:")
    for port in available:
        marker = "board" if is_board_port(port, exclude=_parse_csv(args.exclude)) else "skip"
        print(f"  {port.device}: {marker} desc={port.description} hwid={port.hwid}")

    if args.mode == "relay-cycle":
        if requested:
            ok, errors = validate_requested_ports(available, requested)
            if not ok:
                for error in errors:
                    print(f"[serial-preflight] ERROR: {error}")
                return 1
            print("[serial-preflight] PASS: requested relay-cycle ports are available")
            print("[serial-preflight] next: " + build_relay_cycle_command(requested))
            return 0

        if len(board_ports) < 3:
            print(f"[serial-preflight] ERROR: need 3 board ports for relay-cycle, found {len(board_ports)}")
            if board_ports:
                print("[serial-preflight] found boards: " + ",".join(port.device for port in board_ports))
            return 1
        selected = [port.device for port in board_ports[:3]]
        print("[serial-preflight] PASS: found at least 3 board ports")
        print("[serial-preflight] next: " + build_relay_cycle_command(selected))
        return 0

    print("[serial-preflight] PASS: listed serial ports")
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 serial hardware preflight")
    parser.add_argument("--mode", choices=["list", "relay-cycle"], default="list")
    parser.add_argument("--ports", default="", help="comma-separated ports, e.g. COM16,COM13,COM17")
    parser.add_argument("--exclude", default="COM1", help="comma-separated ports to ignore, default COM1")
    return parser


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
