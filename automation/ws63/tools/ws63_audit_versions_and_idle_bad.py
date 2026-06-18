#!/usr/bin/env python3
"""Audit WS63 board firmware versions and clear mismatched boards to idle."""

from __future__ import annotations

import argparse
import json
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import serial
from serial.tools import list_ports


DEFAULT_EXPECTED_FW = "v4.5.64-minimal"
CFG_JSON_RE = re.compile(r"\[cfg-json\]\s*(\{[^\r\n]*\})")


@dataclass
class BoardResult:
    port: str
    ok: bool = False
    cleared: bool = False
    rebooted: bool = False
    status: dict[str, object] | None = None
    error: str = ""
    log: list[str] = field(default_factory=list)


def com_number(port: str) -> int:
    match = re.fullmatch(r"COM(\d+)", port.upper())
    return int(match.group(1)) if match else 999999


def parse_csv(text: str) -> list[str]:
    return [item.strip().upper() for item in text.split(",") if item.strip()]


def list_ch340_ports(exclude: Iterable[str]) -> list[str]:
    excluded = {item.upper() for item in exclude}
    ports: list[str] = []
    for item in list_ports.comports():
        port = item.device.upper()
        if port in excluded:
            continue
        haystack = f"{item.device} {item.description or ''} {item.hwid or ''}".upper()
        if "CH340" in haystack or "VID:PID=1A86:7523" in haystack:
            ports.append(port)
    return sorted(set(ports), key=com_number)


def write_line(ser: serial.Serial, line: str) -> None:
    ser.write(b"\r\n")
    ser.flush()
    time.sleep(0.03)
    ser.write((line + "\r\n").encode("utf-8"))
    ser.flush()


def drain_text(ser: serial.Serial, seconds: float) -> str:
    end = time.time() + seconds
    chunks: list[str] = []
    while time.time() < end:
        waiting = ser.in_waiting
        if waiting > 0:
            data = ser.read(waiting)
            if data:
                chunks.append(data.decode("utf-8", errors="ignore"))
        time.sleep(0.02)
    return "".join(chunks)


def latest_cfg_json(text: str) -> dict[str, object] | None:
    latest: dict[str, object] | None = None
    for match in CFG_JSON_RE.finditer(text):
        try:
            parsed = json.loads(match.group(1))
        except json.JSONDecodeError:
            continue
        if isinstance(parsed, dict):
            latest = parsed
    return latest


def query_status(ser: serial.Serial, attempts: int, window_s: float, log: list[str]) -> dict[str, object] | None:
    for attempt in range(1, attempts + 1):
        try:
            ser.reset_input_buffer()
        except serial.SerialException:
            pass
        write_line(ser, "cfg status")
        text = drain_text(ser, window_s)
        log.append(f"[attempt {attempt}] cfg status rx_bytes={len(text)}")
        if text:
            log.append(text[-1200:])
        status = latest_cfg_json(text)
        if status is not None:
            return status
    return None


def send_and_wait(ser: serial.Serial, line: str, pattern: str, timeout_s: float, log: list[str]) -> bool:
    rx = re.compile(pattern)
    end = time.time() + timeout_s
    try:
        ser.reset_input_buffer()
    except serial.SerialException:
        pass
    write_line(ser, line)
    collected = ""
    while time.time() < end:
        chunk = drain_text(ser, 0.2)
        if chunk:
            collected += chunk
            if rx.search(collected):
                log.append(f"[tx {line}] matched {pattern}")
                log.append(collected[-1200:])
                return True
    log.append(f"[tx {line}] timeout waiting {pattern}; rx_bytes={len(collected)}")
    if collected:
        log.append(collected[-1200:])
    return False


def audit_port(port: str, args: argparse.Namespace) -> BoardResult:
    result = BoardResult(port=port)
    try:
        with serial.Serial(port, args.baudrate, timeout=0.1, write_timeout=1.0) as ser:
            result.log.append(f"opened {port} baud={args.baudrate}")
            drain_text(ser, args.initial_drain_s)
            status = query_status(ser, args.status_attempts, args.status_window_s, result.log)
            result.status = status
            if status is None:
                result.error = "no cfg-json status"
            elif status.get("fw") == args.expected_fw:
                result.ok = True
                return result
            else:
                result.error = f"firmware mismatch: expected {args.expected_fw}, got {status.get('fw')}"

            if args.no_clear:
                return result
            result.cleared = send_and_wait(ser, "cfg clear", r"\[cfg\]\s+clear ret=0\b", args.clear_timeout_s, result.log)
            if result.cleared:
                write_line(ser, args.reboot_command)
                result.rebooted = True
                result.log.append(f"[tx] {args.reboot_command}")
            return result
    except serial.SerialException as exc:
        result.error = f"serial error: {exc}"
        return result


def write_port_log(log_dir: Path, result: BoardResult) -> None:
    log_dir.mkdir(parents=True, exist_ok=True)
    path = log_dir / f"audit-{result.port}.log"
    lines = [
        f"port: {result.port}",
        f"ok: {result.ok}",
        f"cleared: {result.cleared}",
        f"rebooted: {result.rebooted}",
        f"error: {result.error}",
        "status: " + json.dumps(result.status, ensure_ascii=False, sort_keys=True),
        "",
        *result.log,
    ]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ports", default="", help="comma-separated ports; empty means all CH340 ports")
    parser.add_argument("--exclude", default="COM1", help="comma-separated ports to ignore")
    parser.add_argument("--expected-fw", default=DEFAULT_EXPECTED_FW)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--initial-drain-s", type=float, default=1.0)
    parser.add_argument("--status-window-s", type=float, default=3.0)
    parser.add_argument("--status-attempts", type=int, default=3)
    parser.add_argument("--clear-timeout-s", type=float, default=8.0)
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument("--log-dir", default="logs/hardware/ws63_version_audit")
    parser.add_argument("--no-clear", action="store_true", help="only audit; do not send cfg clear")
    parser.add_argument("--list-only", action="store_true")
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    ports = parse_csv(args.ports) if args.ports else list_ch340_ports(parse_csv(args.exclude))
    if not ports:
        print("no CH340 ports found", file=sys.stderr)
        return 2
    print("ports: " + ",".join(ports))
    if args.list_only:
        return 0

    timestamp = time.strftime("%Y%m%d_%H%M%S")
    log_dir = Path(args.log_dir) / f"{args.expected_fw}_{timestamp}"
    results = [audit_port(port, args) for port in ports]
    for result in results:
        write_port_log(log_dir, result)

    usable = [result.port for result in results if result.ok]
    excluded = [result.port for result in results if not result.ok]
    summary = {
        "expectedFw": args.expected_fw,
        "usablePorts": usable,
        "excludedPorts": excluded,
        "results": [
            {
                "port": result.port,
                "ok": result.ok,
                "cleared": result.cleared,
                "rebooted": result.rebooted,
                "error": result.error,
                "fw": result.status.get("fw") if result.status else None,
                "runtimeConfigured": result.status.get("runtimeConfigured") if result.status else None,
                "runtimeRole": result.status.get("runtimeRole") if result.status else None,
                "routeId": result.status.get("routeId") if result.status else None,
                "selfSuffix": result.status.get("selfSuffix") if result.status else None,
            }
            for result in results
        ],
    }
    (log_dir / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2, ensure_ascii=False))
    return 0 if usable else 1


if __name__ == "__main__":
    raise SystemExit(main())
