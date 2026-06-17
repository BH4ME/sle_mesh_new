#!/usr/bin/env python3
"""Batch-flash WS63 boards and bind members to one leader suffix."""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from dataclasses import dataclass
from typing import Optional

try:
    from automation.ws63.tools.ws63_route_id import route_id_from_suffix
except ModuleNotFoundError:  # pragma: no cover - direct script execution
    from ws63_route_id import route_id_from_suffix

try:
    import serial  # type: ignore
except ImportError as exc:  # pragma: no cover
    raise SystemExit("pyserial is required: pip install pyserial") from exc


@dataclass
class SerialPeer:
    name: str
    port: str
    baudrate: int
    ser: serial.Serial


def _route_id_from_suffix(suffix: int) -> int:
    return route_id_from_suffix(suffix)


def _extract_suffix(text: str) -> Optional[int]:
    label_match = re.search(r"label=[ULM]([0-9A-Fa-f]{4})", text)
    if label_match:
        return int(label_match.group(1), 16)

    json_label_match = re.search(r"selfLabel\"?:\"?[ULM]([0-9A-Fa-f]{4})", text)
    if json_label_match:
        return int(json_label_match.group(1), 16)

    mac_match = re.search(
        r"mac=[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}:([0-9A-Fa-f]{2}):([0-9A-Fa-f]{2})",
        text,
    )
    if mac_match:
        return int(mac_match.group(1) + mac_match.group(2), 16)

    return None


def _parse_ports(text: str) -> list[str]:
    out: list[str] = []
    seen: set[str] = set()
    for raw in text.split(","):
        port = raw.strip()
        if not port or port in seen:
            continue
        out.append(port)
        seen.add(port)
    if not out:
        raise ValueError("member ports list is empty")
    return out


def _open_peer(name: str, port: str, baudrate: int) -> SerialPeer:
    ser = serial.Serial(port, baudrate=baudrate, timeout=0.2)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    return SerialPeer(name=name, port=port, baudrate=baudrate, ser=ser)


def _send_line(peer: SerialPeer, line: str) -> None:
    payload = (line + "\r\n").encode("utf-8")
    peer.ser.write(payload)
    peer.ser.flush()


def _read_available(peer: SerialPeer) -> str:
    data = peer.ser.read(512)
    if not data:
        return ""
    return data.decode("utf-8", errors="ignore")


def _drain(peer: SerialPeer, seconds: float) -> str:
    end = time.time() + seconds
    buf = ""
    while time.time() < end:
        chunk = _read_available(peer)
        if chunk:
            buf += chunk
        time.sleep(0.02)
    return buf


def _wait_regex(peer: SerialPeer, pattern: str, timeout_s: float, note: str) -> str:
    rx = re.compile(pattern)
    end = time.time() + timeout_s
    buf = ""
    while time.time() < end:
        chunk = _read_available(peer)
        if chunk:
            buf += chunk
            if rx.search(buf):
                return buf
        time.sleep(0.02)
    tail = buf[-240:].replace("\r", " ").replace("\n", " ")
    raise RuntimeError(f"{peer.name} timeout waiting for {note}; pattern={pattern}; tail={tail}")


def _detect_leader_suffix(peer: SerialPeer, timeout_s: float, retries: int) -> int:
    for _ in range(max(1, retries)):
        _send_line(peer, "wifi")
        buf = _wait_collect(peer, timeout_s)
        suffix = _extract_suffix(buf)
        if suffix is not None:
            return suffix
    raise RuntimeError("cannot detect leader suffix from serial output")


def _wait_collect(peer: SerialPeer, timeout_s: float) -> str:
    end = time.time() + timeout_s
    buf = ""
    while time.time() < end:
        chunk = _read_available(peer)
        if chunk:
            buf += chunk
        time.sleep(0.02)
    return buf


def _flash_one(flash_script: str, flash_role: str, port: str, flash_gap_s: float) -> None:
    env = os.environ.copy()
    env["WS63_FLASH_NO_CONFIRM"] = "1"
    cmd = [flash_script, "--yes", flash_role, port]
    print(f"[flash-bind] flash role={flash_role} port={port}")
    subprocess.run(cmd, check=True, env=env)
    if flash_gap_s > 0:
        time.sleep(flash_gap_s)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Batch flash WS63 and bind members to one leader MAC suffix")
    parser.add_argument("--leader-port", required=True, help="leader serial port, e.g. /dev/tty.usbserial-10")
    parser.add_argument("--member-ports", required=True, help="comma-separated member ports")
    parser.add_argument("--flash", action="store_true", help="flash before role binding")
    parser.add_argument(
        "--flash-script",
        default="scripts/flash/ws63_flash_team.sh",
        help="flash script path (default scripts/flash/ws63_flash_team.sh)",
    )
    parser.add_argument("--flash-role", default="unified", choices=["leader", "member", "unified"])
    parser.add_argument("--flash-gap-s", type=float, default=0.5)
    parser.add_argument("--post-flash-wait-s", type=float, default=4.0)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument(
        "--leader-suffix",
        default="",
        help="optional leader suffix hex (4 chars); auto-detect from leader serial if omitted",
    )
    parser.add_argument("--detect-timeout-s", type=float, default=4.0)
    parser.add_argument("--detect-retries", type=int, default=3)
    parser.add_argument("--cmd-timeout-s", type=float, default=10.0)
    parser.add_argument("--drain-s", type=float, default=1.2)
    parser.add_argument("--member-join", action="store_true", help="also send join to all members after role member")
    parser.add_argument("--team-id", type=int, default=1)
    parser.add_argument("--channel", type=int, default=17)
    return parser


def run(args: argparse.Namespace) -> int:
    try:
        member_ports = _parse_ports(args.member_ports)
    except ValueError as exc:
        print(f"[flash-bind] FAIL: {exc}")
        return 2

    member_ports = [p for p in member_ports if p != args.leader_port]
    if not member_ports:
        print("[flash-bind] FAIL: no member ports left after removing leader port")
        return 2

    if args.leader_suffix and not re.fullmatch(r"[0-9A-Fa-f]{4}", args.leader_suffix):
        print("[flash-bind] FAIL: --leader-suffix must be 4 hex chars")
        return 2

    if args.flash:
        flash_script = args.flash_script
        if not os.path.exists(flash_script):
            print(f"[flash-bind] FAIL: flash script not found: {flash_script}")
            return 2
        try:
            _flash_one(flash_script, args.flash_role, args.leader_port, args.flash_gap_s)
            for port in member_ports:
                _flash_one(flash_script, args.flash_role, port, args.flash_gap_s)
        except subprocess.CalledProcessError as exc:
            print(f"[flash-bind] FAIL: flash failed, exit={exc.returncode}")
            return 1
        if args.post_flash_wait_s > 0:
            print(f"[flash-bind] wait boot {args.post_flash_wait_s:.1f}s")
            time.sleep(args.post_flash_wait_s)

    peers: list[SerialPeer] = []
    try:
        leader = _open_peer("leader", args.leader_port, args.baudrate)
        peers.append(leader)
        members = []
        for idx, port in enumerate(member_ports, start=1):
            peer = _open_peer(f"member{idx}", port, args.baudrate)
            peers.append(peer)
            members.append(peer)

        for peer in peers:
            _drain(peer, args.drain_s)

        if args.leader_suffix:
            leader_suffix = int(args.leader_suffix, 16)
        else:
            leader_suffix = _detect_leader_suffix(leader, args.detect_timeout_s, args.detect_retries)
        leader_id = _route_id_from_suffix(leader_suffix)
        print(f"[flash-bind] leader_suffix={leader_suffix:04X} leader_id={leader_id}")

        _send_line(leader, "role leader")
        _wait_regex(leader, r"role leader ret=0", args.cmd_timeout_s, "role leader")

        for peer in members:
            _send_line(peer, f"role member {leader_suffix:04X}")
            _wait_regex(peer, r"role member leader_suffix=[0-9A-Fa-f]{4} ret=0", args.cmd_timeout_s, "role member")
            if args.member_join:
                _send_line(peer, f"join {args.team_id} {leader_id} {args.channel}")
                _wait_regex(peer, r"join team=.* ret=0", args.cmd_timeout_s, "member join")

        print(
            "[flash-bind] PASS: leader=%s suffix=%04X members=%s"
            % (args.leader_port, leader_suffix, ",".join(member_ports))
        )
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"[flash-bind] FAIL: {exc}")
        return 1
    finally:
        for peer in peers:
            try:
                peer.ser.close()
            except Exception:  # noqa: BLE001
                pass


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
