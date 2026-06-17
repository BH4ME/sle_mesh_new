#!/usr/bin/env python3
"""Probe WS63 ROM handshake against RTS/DTR reset sequences without flashing."""

from __future__ import annotations

import argparse
import logging
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Optional

import serial

if __package__ in {None, ""}:
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

try:
    from .ws63_auto_burn import (
        ControlStep,
        ResetConfig,
        open_serial_for_burn,
        parse_bool,
        parse_control_sequence,
        perform_auto_reset,
        restore_idle_control_lines,
    )
except ImportError:  # pragma: no cover - direct script execution
    from ws63_auto_burn import (
        ControlStep,
        ResetConfig,
        open_serial_for_burn,
        parse_bool,
        parse_control_sequence,
        perform_auto_reset,
        restore_idle_control_lines,
    )

try:
    from xf_burn_tools.ws63flash import CMD_HANDSHAKE, WS63E_FLASHINFO, Ws63BurnTools
except ModuleNotFoundError as exc:  # pragma: no cover
    raise SystemExit("xf_burn_tools is required for WS63 ROM handshake probing") from exc


ACK = b"\xEF\xBE\xAD\xDE\x0C\x00\xE1\x1E"


@dataclass(frozen=True)
class ProbeCandidate:
    name: str
    idle_rts: bool
    idle_dtr: bool
    assert_after_open: bool
    hold_after_reset: bool
    sequence: tuple[ControlStep, ...]


def _candidate(
    name: str,
    *,
    idle_rts: bool,
    idle_dtr: bool,
    assert_after_open: bool,
    hold_after_reset: bool,
    sequence: str,
) -> ProbeCandidate:
    return ProbeCandidate(
        name=name,
        idle_rts=idle_rts,
        idle_dtr=idle_dtr,
        assert_after_open=assert_after_open,
        hold_after_reset=hold_after_reset,
        sequence=tuple(parse_control_sequence(sequence)),
    )


def default_candidates() -> list[ProbeCandidate]:
    return [
        _candidate(
            "old_rts_only_no_assert_no_hold",
            idle_rts=False,
            idle_dtr=False,
            assert_after_open=False,
            hold_after_reset=False,
            sequence="rts=0:0.25;rts=1:0.5",
        ),
        _candidate(
            "old_rts_only_assert_no_hold",
            idle_rts=False,
            idle_dtr=False,
            assert_after_open=True,
            hold_after_reset=False,
            sequence="rts=0:0.25;rts=1:0.5",
        ),
        _candidate(
            "old_rts_only_no_assert_hold",
            idle_rts=False,
            idle_dtr=False,
            assert_after_open=False,
            hold_after_reset=True,
            sequence="rts=0:0.25;rts=1:0.5",
        ),
        _candidate(
            "old_rts_only_assert_hold",
            idle_rts=False,
            idle_dtr=False,
            assert_after_open=True,
            hold_after_reset=True,
            sequence="rts=0:0.25;rts=1:0.5",
        ),
        _candidate(
            "reverse_rts_only_no_assert_no_hold",
            idle_rts=False,
            idle_dtr=False,
            assert_after_open=False,
            hold_after_reset=False,
            sequence="rts=1:0.25;rts=0:0.5",
        ),
        _candidate(
            "idle_dtr1_rts_only_no_assert_no_hold",
            idle_rts=False,
            idle_dtr=True,
            assert_after_open=False,
            hold_after_reset=False,
            sequence="rts=0:0.25;rts=1:0.5",
        ),
    ]


def _final_control_state(candidate: ProbeCandidate) -> tuple[bool, bool]:
    rts = candidate.idle_rts
    dtr = candidate.idle_dtr
    for step in candidate.sequence:
        if step.rts is not None:
            rts = step.rts
        if step.dtr is not None:
            dtr = step.dtr
    return rts, dtr


def _send_handshake(ser, baudrate: int) -> None:
    tool = Ws63BurnTools(getattr(ser, "port", ""), baudrate)
    tool.ser = ser
    WS63E_FLASHINFO[CMD_HANDSHAKE]["data"][0:4] = baudrate.to_bytes(4, "little")
    tool.ws63SendCmddef(WS63E_FLASHINFO[CMD_HANDSHAKE])


def wait_for_handshake(ser, baudrate: int, timeout_s: float, interval_s: float) -> tuple[bool, bytes, int]:
    deadline = time.time() + timeout_s
    seen = bytearray()
    attempts = 0
    while time.time() < deadline:
        attempts += 1
        _send_handshake(ser, baudrate)
        data = ser.read_all()
        if data:
            seen.extend(data)
            if ACK in seen:
                return True, bytes(seen), attempts
        if interval_s > 0:
            time.sleep(interval_s)
    return False, bytes(seen), attempts


def _format_bytes(data: bytes, limit: int = 32) -> str:
    if not data:
        return "-"
    clipped = data[-limit:]
    text = clipped.hex(" ")
    if len(data) > limit:
        return "... " + text
    return text


def probe_candidate(
    port: str,
    baudrate: int,
    candidate: ProbeCandidate,
    *,
    timeout_s: float,
    interval_s: float,
) -> bool:
    print(
        "CASE name={name} idle_rts={rts} idle_dtr={dtr} assert_after_open={assert_open} "
        "hold_after_reset={hold}".format(
            name=candidate.name,
            rts=int(candidate.idle_rts),
            dtr=int(candidate.idle_dtr),
            assert_open=int(candidate.assert_after_open),
            hold=int(candidate.hold_after_reset),
        ),
        flush=True,
    )
    ser = open_serial_for_burn(
        port,
        115200,
        timeout=0.05,
        idle_rts=candidate.idle_rts,
        idle_dtr=candidate.idle_dtr,
        assert_after_open=candidate.assert_after_open,
    )
    try:
        print(
            "OPEN rts={rts} dtr={dtr} cts={cts} dsr={dsr}".format(
                rts=int(ser.rts),
                dtr=int(ser.dtr),
                cts=int(ser.cts),
                dsr=int(ser.dsr),
            ),
            flush=True,
        )
        try:
            ser.reset_input_buffer()
            ser.reset_output_buffer()
        except serial.SerialException:
            logging.debug("Ignoring buffer reset failure", exc_info=True)

        perform_auto_reset(
            ser,
            ResetConfig(
                command="",
                fallback_command="",
                compatibility_command="",
                sequence=candidate.sequence,
            ),
            log_fn=lambda message: print(message, flush=True),
        )
        if candidate.hold_after_reset:
            hold_rts, hold_dtr = _final_control_state(candidate)
            restore_idle_control_lines(
                ser,
                hold_rts,
                hold_dtr,
                log_fn=lambda message: print(message, flush=True),
                context="Hold after reset",
            )
        ok, seen, attempts = wait_for_handshake(ser, baudrate, timeout_s, interval_s)
        print(
            "RESULT name={name} ok={ok} attempts={attempts} bytes={count} tail={tail}".format(
                name=candidate.name,
                ok=int(ok),
                attempts=attempts,
                count=len(seen),
                tail=_format_bytes(seen),
            ),
            flush=True,
        )
        return ok
    finally:
        ser.close()


def single_candidate_from_args(args: argparse.Namespace) -> ProbeCandidate:
    return _candidate(
        args.name,
        idle_rts=args.idle_rts,
        idle_dtr=args.idle_dtr,
        assert_after_open=args.assert_after_open,
        hold_after_reset=args.hold_after_reset,
        sequence=args.control_sequence,
    )


def iter_candidates(args: argparse.Namespace) -> Iterable[ProbeCandidate]:
    if args.control_sequence:
        yield single_candidate_from_args(args)
        return
    yield from default_candidates()


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Probe WS63 ROM handshake without flashing")
    parser.add_argument("-p", "--port", required=True, help="serial port, for example COM14")
    parser.add_argument("-b", "--baudrate", type=int, default=115200, help="target ROM download baudrate")
    parser.add_argument("--timeout", type=float, default=4.0, help="handshake wait per case, seconds")
    parser.add_argument("--interval", type=float, default=0.05, help="delay between handshake frames, seconds")
    parser.add_argument("--gap", type=float, default=0.8, help="delay between cases, seconds")
    parser.add_argument("--trials", type=int, default=1, help="repeat each case this many times")
    parser.add_argument("--name", default="manual", help="manual candidate name when --control-sequence is used")
    parser.add_argument("--idle-rts", type=parse_bool, default=False)
    parser.add_argument("--idle-dtr", type=parse_bool, default=False)
    parser.add_argument("--assert-after-open", dest="assert_after_open", action="store_true", default=False)
    parser.add_argument("--no-assert-after-open", dest="assert_after_open", action="store_false")
    parser.add_argument("--hold-after-reset", dest="hold_after_reset", action="store_true", default=False)
    parser.add_argument("--no-hold-after-reset", dest="hold_after_reset", action="store_false")
    parser.add_argument(
        "--control-sequence",
        default="",
        help="run one manual candidate, e.g. 'rts=0:0.25;rts=1:0.5'",
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.timeout <= 0:
        raise SystemExit("--timeout must be > 0")
    if args.interval < 0:
        raise SystemExit("--interval must be >= 0")
    if args.gap < 0:
        raise SystemExit("--gap must be >= 0")
    if args.trials < 1:
        raise SystemExit("--trials must be >= 1")

    any_ok = False
    candidates = list(iter_candidates(args))
    for trial in range(1, args.trials + 1):
        print(f"TRIAL {trial}/{args.trials}", flush=True)
        for index, candidate in enumerate(candidates):
            try:
                any_ok = probe_candidate(
                    args.port,
                    args.baudrate,
                    candidate,
                    timeout_s=args.timeout,
                    interval_s=args.interval,
                ) or any_ok
            except Exception as exc:  # noqa: BLE001
                print(f"ERROR name={candidate.name} type={type(exc).__name__} err={exc}", flush=True)
            if args.gap > 0 and (trial != args.trials or index + 1 != len(candidates)):
                time.sleep(args.gap)
    return 0 if any_ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
