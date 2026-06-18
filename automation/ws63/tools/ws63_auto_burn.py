#!/usr/bin/env python3
"""WS63 firmware burner with a project-level auto-reset hook."""

from __future__ import annotations

import argparse
import logging
import math
import os
import struct
import sys
import time
from dataclasses import dataclass
from typing import Callable, Iterable, List, Optional

import serial
from rich.progress import Progress

try:
    from xf_burn_tools import CRC
    from xf_burn_tools.fwpkg import Fwpkg
    from xf_burn_tools import pymodem as vendor_pymodem
    from xf_burn_tools.pymodem import ymodem_xfer
    from xf_burn_tools.ws63flash import (
        CMD_DOWNLOAD,
        CMD_HANDSHAKE,
        CMD_RST,
        RESET_TIMEOUT,
        UART_READ_TIMEOUT,
        WS63E_FLASHINFO,
        Ws63BurnTools,
    )
    HAVE_XF_BURN_TOOLS = True
except ModuleNotFoundError:
    HAVE_XF_BURN_TOOLS = False
    CRC = None  # type: ignore[assignment]
    Fwpkg = None  # type: ignore[assignment]
    vendor_pymodem = None  # type: ignore[assignment]
    ymodem_xfer = None  # type: ignore[assignment]
    CMD_DOWNLOAD = "download"  # type: ignore[assignment]
    CMD_HANDSHAKE = "handshake"  # type: ignore[assignment]
    CMD_RST = "reset"  # type: ignore[assignment]
    RESET_TIMEOUT = 10.0  # type: ignore[assignment]
    UART_READ_TIMEOUT = 1  # type: ignore[assignment]
    WS63E_FLASHINFO = {}  # type: ignore[assignment]

    class Ws63BurnTools:  # type: ignore[no-redef]
        def __init__(self, com, baudrate):
            self.com = com
            self.baudrate = baudrate


SleepFn = Callable[[float], None]
LogFn = Callable[[str], None]


@dataclass(frozen=True)
class ControlStep:
    dtr: Optional[bool] = None
    rts: Optional[bool] = None
    delay_s: float = 0.0


@dataclass(frozen=True)
class ResetConfig:
    command: str = "reboot"
    fallback_command: str = "reset"
    compatibility_command: str = ""
    command_delay_s: float = 0.3
    command_retries: int = 2
    retry_gap_s: float = 0.2
    send_preamble: bool = True
    sequence: Iterable[ControlStep] = ()


DEFAULT_CONTROL_SEQUENCE = "rts=0,dtr=0:0.05;rts=0,dtr=1:0.12;rts=0,dtr=0:0.05"
DEFAULT_EXPECTED_FW_VERSION = "v4.5.64-minimal"
DEFAULT_YMODEM_PACKET_SIZE = 1024
DEFAULT_YMODEM_TRANSFER_RETRIES = 1
DEFAULT_YMODEM_C_TIMEOUT = 5.0
DEFAULT_IDLE_RTS = False
DEFAULT_IDLE_DTR = False
DEFAULT_SERIAL_WRITE_MODE = "nonblocking-drain"
DEFAULT_SERIAL_WRITE_DRAIN_TIMEOUT = 3.0
DEFAULT_SERIAL_WRITE_POST_GAP = 0.0
DEFAULT_ASSERT_CONTROL_AFTER_OPEN = False
DEFAULT_FLASH_ATTEMPTS = 1
DEFAULT_ROM_PREFLIGHT_TIMEOUT = 1.0
SERIAL_WRITE_TRACE_THRESHOLD = 512
DEFAULT_OPEN_CONTROL_SEQUENCE = ""


def parse_bool(value: str) -> bool:
    normalized = value.strip().lower()
    if normalized in {"1", "true", "on", "yes", "high"}:
        return True
    if normalized in {"0", "false", "off", "no", "low"}:
        return False
    raise ValueError(f"invalid control value: {value}")


def parse_control_sequence(text: str) -> List[ControlStep]:
    steps: List[ControlStep] = []
    if not text.strip():
        return steps

    for raw_step in text.split(";"):
        raw_step = raw_step.strip()
        if not raw_step:
            continue
        controls, sep, delay_text = raw_step.partition(":")
        delay_s = float(delay_text) if sep else 0.0
        dtr: Optional[bool] = None
        rts: Optional[bool] = None
        for raw_part in controls.split(","):
            raw_part = raw_part.strip()
            if not raw_part:
                continue
            name, eq, value = raw_part.partition("=")
            if not eq:
                raise ValueError(f"missing '=' in control step: {raw_part}")
            name = name.strip().lower()
            parsed_value = parse_bool(value)
            if name == "dtr":
                dtr = parsed_value
            elif name == "rts":
                rts = parsed_value
            else:
                raise ValueError(f"unknown control signal: {name}")
        steps.append(ControlStep(dtr=dtr, rts=rts, delay_s=delay_s))
    return steps


def firmware_contains_version(firmware_file: str, expected_version: str) -> bool:
    if not expected_version:
        return True
    needle = expected_version.encode("ascii")
    try:
        with open(firmware_file, "rb") as f:
            return needle in f.read()
    except OSError:
        return False


def wait_for_ymodem_receiver(
    serial_port,
    *,
    receiver_ready: bool = False,
    reuse_cached_ready: bool = False,
    c_timeout_s: float = DEFAULT_YMODEM_C_TIMEOUT,
) -> bool:
    if receiver_ready and reuse_cached_ready:
        logging.info("Using YMODEM receiver C captured with ROM handshake")
        return True
    t0 = time.time()
    while True:
        if serial_port.in_waiting > 0:
            cc = serial_port.read(1)
            if cc == bytes([vendor_pymodem.C]):
                return True
        if time.time() - t0 > c_timeout_s:
            return False


def drain_serial_input(serial_port) -> int:
    drained = 0
    while True:
        try:
            pending = serial_port.in_waiting
        except (AttributeError, OSError, serial.SerialException):
            return drained
        if pending <= 0:
            return drained
        try:
            chunk = serial_port.read(pending)
        except serial.SerialException:
            return drained
        if not chunk:
            return drained
        drained += len(chunk)


def ymodem_xfer_with_packet_size(
    serial_port,
    file_path: str,
    bin_info,
    packet_size: int,
    *,
    receiver_ready: bool = False,
    reuse_cached_ready: bool = False,
    c_timeout_s: float = DEFAULT_YMODEM_C_TIMEOUT,
) -> bool:
    if packet_size == 1024:
        return ymodem_xfer_1024(
            serial_port,
            file_path,
            bin_info,
            receiver_ready=receiver_ready,
            reuse_cached_ready=reuse_cached_ready,
            c_timeout_s=c_timeout_s,
        )
    if packet_size != 128:
        raise ValueError(f"unsupported ymodem packet size: {packet_size}")
    return ymodem_xfer_128(
        serial_port,
        file_path,
        bin_info,
        receiver_ready=receiver_ready,
        reuse_cached_ready=reuse_cached_ready,
        c_timeout_s=c_timeout_s,
    )


def ymodem_xfer_1024(
    serial_port,
    file_path: str,
    bin_info,
    *,
    receiver_ready: bool = False,
    reuse_cached_ready: bool = False,
    c_timeout_s: float = DEFAULT_YMODEM_C_TIMEOUT,
) -> bool:
    file_size = bin_info["length"]
    file_name = bin_info["name"]
    offset = bin_info["offset"]
    total_blk = (file_size + 1023) // 1024
    last_blk = file_size % 1024 if file_size % 1024 else 1024

    if not wait_for_ymodem_receiver(
        serial_port,
        receiver_ready=receiver_ready,
        reuse_cached_ready=reuse_cached_ready,
        c_timeout_s=c_timeout_s,
    ):
        logging.warning("YMODEM receiver C timeout before %s", file_name)
        return False

    logging.debug("YMODEM 1024-byte packets: %s (%s B, %s blocks)", file_name, file_size, total_blk)

    blkbuf = bytearray(133)
    blkbuf[0] = vendor_pymodem.SOH
    blkbuf[1] = 0x00
    blkbuf[2] = 0xFF
    file_name_bytes = file_name.encode()
    file_size_bytes = hex(file_size).encode()
    blkbuf[3:3 + len(file_name_bytes)] = file_name_bytes
    blkbuf[3 + len(file_name_bytes) + 1:3 + len(file_name_bytes) + 1 + len(file_size_bytes)] = file_size_bytes
    blkbuf[131:133] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:131]))
    if vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf) is False:
        logging.warning("YMODEM metadata block failed for %s", file_name)
        return False

    with open(file_path, "rb") as f, Progress() as progress:
        task = progress.add_task("[green]Transferring...", total=total_blk)
        f.seek(offset)
        for i_blk in range(1, total_blk + 1):
            blkbuf = bytearray(1029)
            blkbuf[0] = vendor_pymodem.STX
            blkbuf[1] = i_blk % 0x100
            blkbuf[2] = 0xFF - blkbuf[1]
            rlen = last_blk if i_blk == total_blk else 1024
            blkbuf[3:3 + rlen] = f.read(rlen)
            blkbuf[1027:1029] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:1027]))
            if vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf) is False:
                logging.error("YMODEM 1024-byte transfer failed at %s block %s/%s", file_name, i_blk, total_blk)
                return False
            progress.update(task, advance=1)

    eot_deadline_s = time.time() + 10.0
    serial_port.write(bytes([vendor_pymodem.EOT]))
    while vendor_pymodem.ymodem_wait_ack(serial_port) is False:
        if time.time() > eot_deadline_s:
            logging.warning("YMODEM EOT ACK timeout after %s", file_name)
            return False
        serial_port.write(bytes([vendor_pymodem.EOT]))

    blkbuf = bytearray(133)
    blkbuf[0] = vendor_pymodem.SOH
    blkbuf[1] = 0x00
    blkbuf[2] = 0xFF
    blkbuf[131:133] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:131]))
    if vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf) is False:
        logging.warning("YMODEM finish block failed for %s", file_name)
        return False
    return True


def ymodem_xfer_128(
    serial_port,
    file_path: str,
    bin_info,
    *,
    receiver_ready: bool = False,
    reuse_cached_ready: bool = False,
    c_timeout_s: float = DEFAULT_YMODEM_C_TIMEOUT,
) -> bool:
    file_size = bin_info["length"]
    file_name = bin_info["name"]
    offset = bin_info["offset"]
    total_blk = (file_size + 127) // 128
    last_blk = file_size % 128 if file_size % 128 else 128

    if not wait_for_ymodem_receiver(
        serial_port,
        receiver_ready=receiver_ready,
        reuse_cached_ready=reuse_cached_ready,
        c_timeout_s=c_timeout_s,
    ):
        logging.warning("YMODEM receiver C timeout before %s", file_name)
        return False

    logging.info("YMODEM 128-byte packets: %s (%s B, %s blocks)", file_name, file_size, total_blk)

    blkbuf = bytearray(133)
    blkbuf[0] = vendor_pymodem.SOH
    blkbuf[1] = 0x00
    blkbuf[2] = 0xFF
    file_name_bytes = file_name.encode()
    file_size_bytes = hex(file_size).encode()
    blkbuf[3:3 + len(file_name_bytes)] = file_name_bytes
    blkbuf[3 + len(file_name_bytes) + 1:3 + len(file_name_bytes) + 1 + len(file_size_bytes)] = file_size_bytes
    blkbuf[131:133] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:131]))
    if vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf) is False:
        return False

    with open(file_path, "rb") as f, Progress() as progress:
        task = progress.add_task("[green]Transferring128...", total=total_blk)
        f.seek(offset)
        for i_blk in range(1, total_blk + 1):
            blkbuf = bytearray(133)
            blkbuf[0] = vendor_pymodem.SOH
            blkbuf[1] = i_blk % 0x100
            blkbuf[2] = 0xFF - blkbuf[1]
            rlen = last_blk if i_blk == total_blk else 128
            blkbuf[3:3 + rlen] = f.read(rlen)
            blkbuf[131:133] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:131]))
            if vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf) is False:
                logging.error("YMODEM 128-byte transfer failed at %s block %s/%s", file_name, i_blk, total_blk)
                return False
            progress.update(task, advance=1)

    eot_deadline_s = time.time() + 10.0
    serial_port.write(bytes([vendor_pymodem.EOT]))
    while vendor_pymodem.ymodem_wait_ack(serial_port) is False:
        if time.time() > eot_deadline_s:
            logging.warning("YMODEM EOT ACK timeout after %s", file_name)
            return False
        serial_port.write(bytes([vendor_pymodem.EOT]))

    blkbuf = bytearray(133)
    blkbuf[0] = vendor_pymodem.SOH
    blkbuf[1] = 0x00
    blkbuf[2] = 0xFF
    blkbuf[131:133] = struct.pack(">H", CRC.calc_crc16(blkbuf[3:131]))
    return vendor_pymodem.ymodem_blk_timed_xmit(serial_port, blkbuf)


def install_serial_write_limiter(ser, chunk_size: int, gap_s: float, sleep_fn: SleepFn = time.sleep) -> None:
    if chunk_size <= 0:
        return

    raw_write = ser.write

    def write_limited(data):
        payload = bytes(data)
        total = 0
        for start in range(0, len(payload), chunk_size):
            end = min(start + chunk_size, len(payload))
            total += raw_write(payload[start:end])
            if gap_s > 0.0 and end < len(payload):
                sleep_fn(gap_s)
        return total

    ser.write = write_limited


def install_nonblocking_drain_writer(
    ser,
    *,
    drain_timeout_s: float,
    poll_s: float = 0.005,
    sleep_fn: SleepFn = time.sleep,
) -> None:
    raw_write = ser.write

    def write_nonblocking_drain(data):
        payload = bytes(data)
        expected = len(payload)
        written = raw_write(payload)
        if written != expected:
            raise serial.SerialTimeoutException(f"short nonblocking write: {written}/{expected}")
        deadline = time.time() + drain_timeout_s
        while True:
            out_waiting = ser.out_waiting
            if out_waiting == 0:
                return written
            if time.time() > deadline:
                raise serial.SerialTimeoutException(
                    f"write drain timeout with {out_waiting} byte(s) still queued"
                )
            sleep_fn(poll_s)

    ser.write = write_nonblocking_drain


def install_serial_write_post_gap(
    ser,
    gap_s: float,
    *,
    min_size: int = SERIAL_WRITE_TRACE_THRESHOLD,
    sleep_fn: SleepFn = time.sleep,
) -> None:
    if gap_s <= 0.0:
        return

    raw_write = ser.write

    def write_with_post_gap(data):
        payload = bytes(data)
        written = raw_write(payload)
        if len(payload) >= min_size:
            sleep_fn(gap_s)
        return written

    ser.write = write_with_post_gap


def _format_serial_attr(value) -> str:
    if isinstance(value, bool):
        return str(int(value))
    return str(value)


def describe_serial_state(ser) -> str:
    fields = []
    for name in ("baudrate", "rts", "dtr", "cts", "dsr", "cd", "ri", "in_waiting", "out_waiting"):
        try:
            value = getattr(ser, name)
        except (OSError, serial.SerialException) as exc:
            value = f"<{type(exc).__name__}>"
        fields.append(f"{name}={_format_serial_attr(value)}")
    return " ".join(fields)


def install_serial_write_tracer(
    ser,
    *,
    large_write_threshold: int = SERIAL_WRITE_TRACE_THRESHOLD,
    report_every_large_write: int = 64,
) -> None:
    raw_write = ser.write
    counts = {"writes": 0, "large_writes": 0}

    def write_traced(data):
        payload = bytes(data)
        counts["writes"] += 1
        is_large = len(payload) >= large_write_threshold
        if is_large:
            counts["large_writes"] += 1
        try:
            written = raw_write(payload)
        except Exception as exc:
            logging.error(
                "Serial write failed: write=%s large_write=%s len=%s %s: %s state=%s",
                counts["writes"],
                counts["large_writes"],
                len(payload),
                type(exc).__name__,
                exc,
                describe_serial_state(ser),
            )
            raise
        if is_large and (
            counts["large_writes"] <= 2 or counts["large_writes"] % report_every_large_write == 0
        ):
            logging.debug(
                "Serial write ok: write=%s large_write=%s len=%s written=%s state=%s",
                counts["writes"],
                counts["large_writes"],
                len(payload),
                written,
                describe_serial_state(ser),
            )
        return written

    ser.write = write_traced


class ControlLineGuard:
    def __init__(self, ser, *, context: str) -> None:
        object.__setattr__(self, "_ser", ser)
        object.__setattr__(self, "_context", context)

    def __getattr__(self, name):
        return getattr(self._ser, name)

    def __setattr__(self, name, value) -> None:
        if name in {"rts", "dtr"}:
            self._block(name.upper(), value)
        setattr(self._ser, name, value)

    def _block(self, name: str, value=1) -> None:
        message = (
            f"Unexpected {name} change during {self._context}: requested {name}={int(bool(value))} "
            f"state={describe_serial_state(self._ser)}"
        )
        logging.error(message)
        raise RuntimeError(message)

    def setRTS(self, value=1):
        self._block("RTS", value)

    def setDTR(self, value=1):
        self._block("DTR", value)


def install_control_line_guard(ser, *, context: str):
    return ControlLineGuard(ser, context=context)


def open_serial_for_burn(
    port: str,
    baudrate: int,
    *,
    timeout: float,
    write_timeout: Optional[float] = None,
    idle_rts: bool,
    idle_dtr: bool,
    assert_after_open: bool = DEFAULT_ASSERT_CONTROL_AFTER_OPEN,
    preload_control_lines: bool = True,
):
    ser = serial.Serial()
    ser.port = port
    ser.baudrate = baudrate
    ser.timeout = timeout
    ser.write_timeout = write_timeout
    ser.rtscts = False
    ser.dsrdtr = False
    ser.xonxoff = False
    if preload_control_lines:
        ser.rts = idle_rts
        ser.dtr = idle_dtr
    ser.open()
    if preload_control_lines and assert_after_open:
        ser.setRTS(idle_rts)
        ser.setDTR(idle_dtr)
    return ser


def restore_idle_control_lines(
    ser,
    idle_rts: bool,
    idle_dtr: bool,
    *,
    log_fn: LogFn = logging.info,
    context: str = "Serial idle",
) -> None:
    try:
        rts_matches = ser.rts == idle_rts
    except (AttributeError, OSError, serial.SerialException):
        rts_matches = False
    try:
        dtr_matches = ser.dtr == idle_dtr
    except (AttributeError, OSError, serial.SerialException):
        dtr_matches = False
    if not rts_matches:
        ser.setRTS(idle_rts)
    if not dtr_matches:
        ser.setDTR(idle_dtr)
    log_fn(f"{context}: RTS={int(idle_rts)} DTR={int(idle_dtr)}")


def reset_config_uses_control_lines(reset_config: Optional[ResetConfig]) -> bool:
    if reset_config is None:
        return False
    return any(step.rts is not None or step.dtr is not None for step in reset_config.sequence)


def derive_hold_control_state(
    idle_rts: bool,
    idle_dtr: bool,
    reset_config: Optional[ResetConfig],
) -> tuple[bool, bool]:
    hold_rts = idle_rts
    hold_dtr = idle_dtr
    if reset_config is None:
        return hold_rts, hold_dtr
    for step in reset_config.sequence:
        if step.rts is not None:
            hold_rts = step.rts
        if step.dtr is not None:
            hold_dtr = step.dtr
    return hold_rts, hold_dtr


def perform_auto_reset(
    ser,
    config: ResetConfig,
    *,
    sleep_fn: SleepFn = time.sleep,
    log_fn: LogFn = logging.info,
) -> None:
    command_list: List[str] = []
    for raw_command in (config.command, config.fallback_command, config.compatibility_command):
        command = raw_command.strip()
        if command and command not in command_list:
            command_list.append(command)

    retries = max(1, config.command_retries)
    for attempt in range(retries):
        for command in command_list:
            if config.send_preamble:
                ser.write(b"\r\n")
                ser.flush()
                if config.command_delay_s > 0.0:
                    sleep_fn(min(config.command_delay_s, 0.05))
            payload = command.encode("ascii") + b"\r\n"
            log_fn(f"Auto reset: sending CLI command '{command}'")
            ser.write(payload)
            ser.flush()
            if config.command_delay_s > 0.0:
                sleep_fn(config.command_delay_s)
        if attempt + 1 < retries and config.retry_gap_s > 0.0:
            sleep_fn(config.retry_gap_s)

    for step in config.sequence:
        parts = []
        if step.rts is not None:
            ser.setRTS(step.rts)
            parts.append(f"RTS={int(step.rts)}")
        if step.dtr is not None:
            ser.setDTR(step.dtr)
            parts.append(f"DTR={int(step.dtr)}")
        if parts:
            log_fn("Auto reset: " + " ".join(parts))
        if step.delay_s > 0.0:
            sleep_fn(step.delay_s)

    try:
        ser.reset_input_buffer()
    except serial.SerialException:
        logging.debug("Ignoring reset_input_buffer failure during auto reset", exc_info=True)


def perform_control_sequence(
    ser,
    sequence: Iterable[ControlStep],
    *,
    label: str,
    sleep_fn: SleepFn = time.sleep,
    log_fn: LogFn = logging.info,
) -> None:
    for step in sequence:
        parts = []
        if step.rts is not None:
            ser.setRTS(step.rts)
            parts.append(f"RTS={int(step.rts)}")
        if step.dtr is not None:
            ser.setDTR(step.dtr)
            parts.append(f"DTR={int(step.dtr)}")
        if parts:
            log_fn(f"{label}: " + " ".join(parts))
        if step.delay_s > 0.0:
            sleep_fn(step.delay_s)


class AutoResetWs63BurnTools(Ws63BurnTools):
    def __init__(
        self,
        com: str,
        baudrate: int,
        reset_config: Optional[ResetConfig],
        *,
        wait_timeout_s: float,
        handshake_interval_s: float,
        manual_retry_timeout_s: float,
        legacy_reset_order: bool = False,
        ymodem_packet_size: int = DEFAULT_YMODEM_PACKET_SIZE,
        ymodem_transfer_retries: int = DEFAULT_YMODEM_TRANSFER_RETRIES,
        ymodem_c_timeout_s: float = DEFAULT_YMODEM_C_TIMEOUT,
        serial_write_chunk_size: int = 0,
        serial_write_gap_s: float = 0.0,
        serial_write_mode: str = DEFAULT_SERIAL_WRITE_MODE,
        serial_write_drain_timeout_s: float = DEFAULT_SERIAL_WRITE_DRAIN_TIMEOUT,
        serial_write_post_gap_s: float = DEFAULT_SERIAL_WRITE_POST_GAP,
        idle_rts: bool = DEFAULT_IDLE_RTS,
        idle_dtr: bool = DEFAULT_IDLE_DTR,
        assert_control_after_open: bool = DEFAULT_ASSERT_CONTROL_AFTER_OPEN,
        preload_control_lines: bool = True,
        open_control_sequence: Iterable[ControlStep] = (),
        skip_reset_if_rom_active: bool = False,
        rom_preflight_timeout_s: float = DEFAULT_ROM_PREFLIGHT_TIMEOUT,
        reuse_handshake_receiver_c: bool = False,
        drain_handshake_tail: bool = False,
    ) -> None:
        super().__init__(com, baudrate)
        self.reset_config = reset_config
        self.wait_timeout_s = wait_timeout_s
        self.handshake_interval_s = handshake_interval_s
        self.manual_retry_timeout_s = manual_retry_timeout_s
        self.legacy_reset_order = legacy_reset_order
        self.ymodem_packet_size = ymodem_packet_size
        self.ymodem_transfer_retries = ymodem_transfer_retries
        self.ymodem_c_timeout_s = ymodem_c_timeout_s
        self.serial_write_chunk_size = serial_write_chunk_size
        self.serial_write_gap_s = serial_write_gap_s
        self.serial_write_mode = serial_write_mode
        self.serial_write_drain_timeout_s = serial_write_drain_timeout_s
        self.serial_write_post_gap_s = serial_write_post_gap_s
        self.idle_rts = idle_rts
        self.idle_dtr = idle_dtr
        self.assert_control_after_open = assert_control_after_open
        self.preload_control_lines = preload_control_lines
        self.open_control_sequence = tuple(open_control_sequence)
        self.skip_reset_if_rom_active = skip_reset_if_rom_active
        self.rom_preflight_timeout_s = rom_preflight_timeout_s
        self.reuse_handshake_receiver_c = reuse_handshake_receiver_c
        self.drain_handshake_tail = drain_handshake_tail
        self.hold_rts, self.hold_dtr = derive_hold_control_state(idle_rts, idle_dtr, reset_config)
        self._ymodem_receiver_ready = False

    def _restore_hold_controls(self, context: str) -> None:
        restore_idle_control_lines(self.ser, self.hold_rts, self.hold_dtr, context=context)

    def _try_wait_for_handshake(self, timeout_s: float) -> bool:
        t0 = time.time()
        next_handshake_s = 0.0
        ack = b"\xEF\xBE\xAD\xDE\x0C\x00\xE1\x1E"
        ack_window = bytearray()
        while True:
            elapsed_s = time.time() - t0
            if elapsed_s > timeout_s:
                return False

            if elapsed_s >= next_handshake_s:
                WS63E_FLASHINFO[CMD_HANDSHAKE]["data"][0:4] = self.baudrate.to_bytes(4, "little")
                self.ws63SendCmddef(WS63E_FLASHINFO[CMD_HANDSHAKE])
                next_handshake_s = elapsed_s + self.handshake_interval_s

            if self.ser.in_waiting > 0:
                data = self.ser.read(1)
                if not data:
                    continue
                ack_window += data
                if len(ack_window) > len(ack):
                    del ack_window[: len(ack_window) - len(ack)]
                if bytes(ack_window) == ack:
                    logging.info("ROM handshake ACK: %s", describe_serial_state(self.ser))
                    if self.ser.baudrate != self.baudrate:
                        logging.info("Switching serial baudrate to %s", self.baudrate)
                        self.ser.baudrate = self.baudrate
                        logging.info("After baudrate switch: %s", describe_serial_state(self.ser))
                    if self.reuse_handshake_receiver_c:
                        self._ymodem_receiver_ready = True
                    if self.drain_handshake_tail:
                        drained = drain_serial_input(self.ser)
                        if drained > 0:
                            logging.info("Drained ROM handshake tail before YMODEM: %s bytes", drained)
                    return True

            time.sleep(0.005)

    def _ymodem_transfer_with_retries(self, firmware: str, bin_info, label: str) -> bool:
        attempts = max(1, self.ymodem_transfer_retries)
        for attempt in range(1, attempts + 1):
            if attempts > 1:
                logging.info("YMODEM transfer attempt %s/%s for %s", attempt, attempts, label)
            ret = ymodem_xfer_with_packet_size(
                self.ser,
                firmware,
                bin_info,
                self.ymodem_packet_size,
                receiver_ready=self._ymodem_receiver_ready,
                reuse_cached_ready=self.reuse_handshake_receiver_c,
                c_timeout_s=self.ymodem_c_timeout_s,
            )
            self._ymodem_receiver_ready = False
            if ret is not False:
                return True
            logging.warning(
                "YMODEM transfer failed for %s attempt %s/%s: %s",
                label,
                attempt,
                attempts,
                describe_serial_state(self.ser),
            )
            if attempt < attempts:
                time.sleep(0.5)
        return False

    def flash(self, name) -> bool:  # noqa: C901 - mirrors xf_burn_tools to keep its protocol behavior.
        self.ser = open_serial_for_burn(
            self.com,
            115200,
            timeout=1,
            write_timeout=0 if self.serial_write_mode == "nonblocking-drain" else None,
            idle_rts=self.idle_rts,
            idle_dtr=self.idle_dtr,
            assert_after_open=self.assert_control_after_open,
            preload_control_lines=self.preload_control_lines,
        )
        if self.preload_control_lines:
            logging.info("Serial opened with idle RTS=%s DTR=%s", int(self.idle_rts), int(self.idle_dtr))
        else:
            logging.info("Serial opened without explicit RTS/DTR preload")
        if self.open_control_sequence:
            perform_control_sequence(self.ser, self.open_control_sequence, label="Open control")
            restore_idle_control_lines(
                self.ser,
                self.idle_rts,
                self.idle_dtr,
                context="Open control-line release",
            )
        if self.serial_write_mode == "nonblocking-drain":
            logging.info("Serial write mode: nonblocking-drain")
            install_nonblocking_drain_writer(self.ser, drain_timeout_s=self.serial_write_drain_timeout_s)
        install_serial_write_limiter(self.ser, self.serial_write_chunk_size, self.serial_write_gap_s)
        if self.serial_write_post_gap_s > 0.0:
            logging.info("Serial write post-gap for large writes: %.3f s", self.serial_write_post_gap_s)
            install_serial_write_post_gap(self.ser, self.serial_write_post_gap_s)
        install_serial_write_tracer(self.ser)
        if self.reset_config is not None and self.legacy_reset_order:
            perform_auto_reset(self.ser, self.reset_config)

        self.fwpkg = Fwpkg(name)
        loaderboot = None
        for bin_info in self.fwpkg.bin_infos:
            if bin_info["type"] == 0:
                loaderboot = bin_info
                break
        if not loaderboot:
            logging.error("Required loaderboot not found in fwpkg!")
            return False

        self.fwpkg.show()

        handshake_ready = False
        if self.reset_config is not None and not self.legacy_reset_order and self.skip_reset_if_rom_active:
            logging.info("Checking whether ROM handshake is already active before reset...")
            handshake_ready = self._try_wait_for_handshake(self.rom_preflight_timeout_s)
            if handshake_ready:
                logging.info("ROM handshake already active; skipping reset command")

        if self.reset_config is not None and not self.legacy_reset_order and not handshake_ready:
            perform_auto_reset(self.ser, self.reset_config)
        if reset_config_uses_control_lines(self.reset_config):
            restore_idle_control_lines(
                self.ser,
                self.idle_rts,
                self.idle_dtr,
                context="Download control-line release",
            )
        self.ser = install_control_line_guard(self.ser, context="WS63 download")
        logging.info("Download control-line guard active: %s", describe_serial_state(self.ser))

        if handshake_ready:
            logging.info("Using ROM handshake detected before reset")
        else:
            logging.info("Waiting for device reset...")
            if not self._try_wait_for_handshake(self.wait_timeout_s):
                logging.warning("Auto handshake timeout. Please press reset / BOOT+RESET now...")
                if not self._try_wait_for_handshake(self.manual_retry_timeout_s):
                    logging.warning("Timeout while waiting for device reset")
                    logging.error(
                        "No ROM handshake after reset. If the port also has no CLI reply, "
                        "power-cycle or press reset, then rerun with the appropriate software-reset "
                        "or hardware-reset path for that board."
                    )
                    return False
        logging.info("Establishing ymodem session...")

        time.sleep(0.5)
        logging.info(f"Transferring {loaderboot['name']}...")
        if not self._ymodem_transfer_with_retries(name, loaderboot, loaderboot["name"]):
            logging.error(f"Error transferring {loaderboot['name']}")
            return False

        self.uartReadUntilMagic()

        for bin_info in self.fwpkg.bin_infos:
            if bin_info["type"] != 1:
                continue
            logging.info(f"Transferring {bin_info['name']}...")
            eras_size = math.ceil(bin_info["length"] / 8192.0) * 0x2000
            WS63E_FLASHINFO[CMD_DOWNLOAD]["data"][0:4] = bin_info["burn_addr"].to_bytes(4, "little")
            WS63E_FLASHINFO[CMD_DOWNLOAD]["data"][4:8] = bin_info["length"].to_bytes(4, "little")
            WS63E_FLASHINFO[CMD_DOWNLOAD]["data"][8:12] = int(eras_size).to_bytes(4, "little")
            self.ws63SendCmddef(WS63E_FLASHINFO[CMD_DOWNLOAD])
            self.uartReadUntilMagic()
            if not self._ymodem_transfer_with_retries(name, bin_info, bin_info["name"]):
                logging.error(f"Error transferring {bin_info['name']}")
                return False
            time.sleep(0.1)
        logging.info("Done. Reseting device...")
        self.ws63SendCmddef(WS63E_FLASHINFO[CMD_RST])
        self.uartReadUntilMagic()
        self.ser.close()
        return True


def build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Burn WS63 firmware and try to reset the board automatically.")
    parser.add_argument("firmware_file")
    parser.add_argument("-p", "--port", help="serial port, for example /dev/tty.usbserial-10")
    parser.add_argument("-b", "--baudrate", type=int, default=115200, help="burn baudrate")
    parser.add_argument("-v", "--verbose", action="store_true", help="print debug logs")
    parser.add_argument("-s", "--show", action="store_true", help="show firmware information only")
    parser.add_argument("--no-auto-reset", action="store_true", help="disable CLI/DTR/RTS reset before burn")
    parser.add_argument("--reset-command", default="reboot", help="CLI command to send before handshaking")
    parser.add_argument("--reset-command-fallback", default="reset", help="fallback CLI command before handshaking")
    parser.add_argument(
        "--compat-reset-command",
        default="AT+RST",
        help="compatibility reset command for AT-style firmware",
    )
    parser.add_argument(
        "--no-compat-reset-command",
        action="store_true",
        help="do not send compatibility reset command",
    )
    parser.add_argument(
        "--no-reset-command-fallback",
        action="store_true",
        help="do not send fallback CLI reset command",
    )
    parser.add_argument("--no-reset-command", action="store_true", help="do not send a CLI reboot command")
    parser.add_argument(
        "--no-reset-preamble",
        action="store_true",
        help="do not send a blank CRLF before each CLI reset command",
    )
    parser.add_argument(
        "--legacy-reset-order",
        action="store_true",
        help="send CLI reset commands before parsing/showing the fwpkg, matching the historical COM16 flow",
    )
    parser.add_argument("--reset-command-delay", type=float, default=0.3, help="delay after reset command, seconds")
    parser.add_argument(
        "--reset-command-retries",
        type=int,
        default=2,
        help="how many times to send reset command sequence before burn handshake",
    )
    parser.add_argument(
        "--reset-command-retry-gap",
        type=float,
        default=0.2,
        help="delay between repeated reset command sequences, seconds",
    )
    parser.add_argument(
        "--software-reset-only",
        action="store_true",
        help="do not drive DTR/RTS, use serial CLI reset commands only",
    )
    parser.add_argument(
        "--control-sequence",
        default=DEFAULT_CONTROL_SEQUENCE,
        help="DTR/RTS sequence, for example 'rts=0,dtr=1:0.1;rts=0,dtr=0:0.1'",
    )
    parser.add_argument(
        "--idle-rts",
        type=parse_bool,
        default=DEFAULT_IDLE_RTS,
        help="RTS state to preload before opening the serial port and restore before download",
    )
    parser.add_argument(
        "--idle-dtr",
        type=parse_bool,
        default=DEFAULT_IDLE_DTR,
        help="DTR state to preload before opening the serial port and restore before download",
    )
    parser.add_argument(
        "--assert-control-after-open",
        dest="assert_control_after_open",
        action="store_true",
        default=DEFAULT_ASSERT_CONTROL_AFTER_OPEN,
        help="send SETRTS/SETDTR after opening the port; off by default to avoid extra EN pulses",
    )
    parser.add_argument(
        "--no-assert-control-after-open",
        dest="assert_control_after_open",
        action="store_false",
        help="do not send SETRTS/SETDTR after opening the port",
    )
    parser.add_argument(
        "--control-line-preload",
        dest="preload_control_lines",
        action="store_true",
        default=True,
        help="preload RTS/DTR before opening the port",
    )
    parser.add_argument(
        "--no-control-line-preload",
        dest="preload_control_lines",
        action="store_false",
        help="open the port without explicitly writing RTS/DTR first",
    )
    parser.add_argument(
        "--open-control-sequence",
        default=DEFAULT_OPEN_CONTROL_SEQUENCE,
        help="optional DTR/RTS sequence immediately after opening the port, before package parsing",
    )
    parser.add_argument(
        "--wait-timeout",
        type=float,
        default=float(RESET_TIMEOUT),
        help="initial handshake wait timeout in seconds",
    )
    parser.add_argument(
        "--manual-retry-timeout",
        type=float,
        default=20.0,
        help="extra wait window after auto reset timeout for manual reset",
    )
    parser.add_argument(
        "--handshake-interval",
        type=float,
        default=0.05,
        help="delay between repeated handshake probes in seconds",
    )
    parser.add_argument(
        "--ymodem-packet-size",
        type=int,
        choices=[128, 1024],
        default=DEFAULT_YMODEM_PACKET_SIZE,
        help="YMODEM data packet size; 1024 matches the vendor default, 128 is slower but useful for marginal links",
    )
    parser.add_argument(
        "--ymodem-transfer-retries",
        type=int,
        default=DEFAULT_YMODEM_TRANSFER_RETRIES,
        help="retry each complete YMODEM image transfer this many times without changing packet size",
    )
    parser.add_argument(
        "--ymodem-c-timeout",
        type=float,
        default=DEFAULT_YMODEM_C_TIMEOUT,
        help="seconds to wait for the receiver's YMODEM C before each image transfer",
    )
    parser.add_argument(
        "--serial-write-chunk-size",
        type=int,
        default=0,
        help="split serial writes into this many bytes; 0 disables chunking",
    )
    parser.add_argument(
        "--serial-write-gap",
        type=float,
        default=0.0,
        help="delay between serial write chunks, seconds",
    )
    parser.add_argument(
        "--serial-write-mode",
        choices=["blocking", "nonblocking-drain"],
        default=DEFAULT_SERIAL_WRITE_MODE,
        help="serial write behavior; nonblocking-drain keeps 1024-byte YMODEM packets but avoids Win32 blocking writes",
    )
    parser.add_argument(
        "--serial-write-drain-timeout",
        type=float,
        default=DEFAULT_SERIAL_WRITE_DRAIN_TIMEOUT,
        help="timeout for nonblocking-drain TX queue drain, seconds",
    )
    parser.add_argument(
        "--serial-write-post-gap",
        type=float,
        default=DEFAULT_SERIAL_WRITE_POST_GAP,
        help="sleep after each large completed serial write without splitting the write, seconds",
    )
    parser.add_argument(
        "--flash-attempts",
        type=int,
        default=DEFAULT_FLASH_ATTEMPTS,
        help="retry the entire open/reset/handshake/burn session this many times",
    )
    parser.add_argument(
        "--skip-reset-if-rom-active",
        action="store_true",
        help="probe for an already-active ROM handshake before sending reset commands",
    )
    parser.add_argument(
        "--rom-preflight-timeout",
        type=float,
        default=DEFAULT_ROM_PREFLIGHT_TIMEOUT,
        help="ROM preflight handshake timeout in seconds when --skip-reset-if-rom-active is used",
    )
    parser.add_argument(
        "--reuse-handshake-receiver-c",
        action="store_true",
        help="manual recovery: reuse a YMODEM C byte that arrives in the ROM handshake read",
    )
    parser.add_argument(
        "--drain-handshake-tail",
        action="store_true",
        help="manual recovery: discard bytes left after ROM handshake before starting YMODEM",
    )
    parser.add_argument(
        "--expected-version",
        default=os.environ.get("EXPECTED_FW_VERSION", DEFAULT_EXPECTED_FW_VERSION),
        help="refuse to burn unless firmware package contains this version string; empty disables the check",
    )
    return parser


def main(argv: Optional[List[str]] = None) -> int:
    parser = build_arg_parser()
    args = parser.parse_args(argv)

    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO, format="%(message)s")

    if args.show:
        if not HAVE_XF_BURN_TOOLS:
            logging.error("xf_burn_tools is not installed. Please install vendor burn tools to inspect firmware.")
            return 2
        Fwpkg(args.firmware_file).show()
        return 0
    if not args.port:
        parser.error("the following arguments are required for burning: -p/--port")
    if args.reset_command_retries < 1:
        parser.error("--reset-command-retries must be >= 1")
    if args.reset_command_delay < 0.0:
        parser.error("--reset-command-delay must be >= 0")
    if args.reset_command_retry_gap < 0.0:
        parser.error("--reset-command-retry-gap must be >= 0")
    if args.wait_timeout <= 0.0:
        parser.error("--wait-timeout must be > 0")
    if args.manual_retry_timeout < 0.0:
        parser.error("--manual-retry-timeout must be >= 0")
    if args.handshake_interval < 0.0:
        parser.error("--handshake-interval must be >= 0")
    if args.serial_write_chunk_size < 0:
        parser.error("--serial-write-chunk-size must be >= 0")
    if args.ymodem_transfer_retries < 1:
        parser.error("--ymodem-transfer-retries must be >= 1")
    if args.ymodem_c_timeout <= 0.0:
        parser.error("--ymodem-c-timeout must be > 0")
    if args.serial_write_gap < 0.0:
        parser.error("--serial-write-gap must be >= 0")
    if args.serial_write_drain_timeout <= 0.0:
        parser.error("--serial-write-drain-timeout must be > 0")
    if args.serial_write_post_gap < 0.0:
        parser.error("--serial-write-post-gap must be >= 0")
    if args.flash_attempts < 1:
        parser.error("--flash-attempts must be >= 1")
    if args.rom_preflight_timeout <= 0.0:
        parser.error("--rom-preflight-timeout must be > 0")
    if args.expected_version and not firmware_contains_version(args.firmware_file, args.expected_version):
        logging.error("Firmware package does not contain expected version: %s", args.expected_version)
        logging.error("Refusing to flash stale package: %s", args.firmware_file)
        logging.error("Use --expected-version '' or EXPECTED_FW_VERSION= to override intentionally.")
        return 3

    if not HAVE_XF_BURN_TOOLS:
        logging.error("xf_burn_tools is not installed. Please install vendor burn tools to use flashing.")
        return 2

    reset_config = None
    try:
        open_control_sequence = parse_control_sequence(args.open_control_sequence)
    except ValueError as exc:
        parser.error(str(exc))
    if not args.no_auto_reset:
        sequence = []
        if not args.software_reset_only:
            try:
                sequence = parse_control_sequence(args.control_sequence)
            except ValueError as exc:
                parser.error(str(exc))
        reset_config = ResetConfig(
            command="" if args.no_reset_command else args.reset_command,
            fallback_command="" if args.no_reset_command_fallback else args.reset_command_fallback,
            compatibility_command="" if args.no_compat_reset_command else args.compat_reset_command,
            command_delay_s=args.reset_command_delay,
            command_retries=args.reset_command_retries,
            retry_gap_s=args.reset_command_retry_gap,
            send_preamble=not args.no_reset_preamble,
            sequence=sequence,
        )

    for attempt in range(1, args.flash_attempts + 1):
        if args.flash_attempts > 1:
            logging.info("Flash session attempt %s/%s", attempt, args.flash_attempts)
        tools = AutoResetWs63BurnTools(
            args.port,
            args.baudrate,
            reset_config,
            wait_timeout_s=args.wait_timeout,
            handshake_interval_s=args.handshake_interval,
            manual_retry_timeout_s=args.manual_retry_timeout,
            legacy_reset_order=args.legacy_reset_order,
            ymodem_packet_size=args.ymodem_packet_size,
            ymodem_transfer_retries=args.ymodem_transfer_retries,
            ymodem_c_timeout_s=args.ymodem_c_timeout,
            serial_write_chunk_size=args.serial_write_chunk_size,
            serial_write_gap_s=args.serial_write_gap,
            serial_write_mode=args.serial_write_mode,
            serial_write_drain_timeout_s=args.serial_write_drain_timeout,
            serial_write_post_gap_s=args.serial_write_post_gap,
            idle_rts=args.idle_rts,
            idle_dtr=args.idle_dtr,
            assert_control_after_open=args.assert_control_after_open,
            preload_control_lines=args.preload_control_lines,
            open_control_sequence=open_control_sequence,
            skip_reset_if_rom_active=args.skip_reset_if_rom_active,
            rom_preflight_timeout_s=args.rom_preflight_timeout,
            reuse_handshake_receiver_c=args.reuse_handshake_receiver_c,
            drain_handshake_tail=args.drain_handshake_tail,
        )
        try:
            if tools.flash(args.firmware_file):
                return 0
        finally:
            ser = getattr(tools, "ser", None)
            close_fn = getattr(ser, "close", None)
            if callable(close_fn):
                try:
                    close_fn()
                except serial.SerialException:
                    logging.debug("Ignoring serial close failure after flash attempt", exc_info=True)
        if attempt < args.flash_attempts:
            logging.warning("Flash session attempt %s/%s failed; restarting full burn session", attempt, args.flash_attempts)
            time.sleep(1.0)
    return 1


if __name__ == "__main__":
    sys.exit(main())
