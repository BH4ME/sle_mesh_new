import os
import tempfile
import unittest
from unittest import mock

from automation.ws63.tools import ws63_auto_burn


class FakeSerial:
    def __init__(self):
        self.ops = []

    def write(self, data):
        self.ops.append(("write", data))
        return len(data)

    def flush(self):
        self.ops.append(("flush",))

    def reset_input_buffer(self):
        self.ops.append(("reset_input_buffer",))

    def setDTR(self, value):
        self.ops.append(("dtr", value))

    def setRTS(self, value):
        self.ops.append(("rts", value))


class FakeOpenSerial:
    def __init__(self):
        self.ops = []
        self.port = None
        self.baudrate = None
        self.timeout = None
        self.write_timeout = "unset"
        self.rtscts = None
        self.dsrdtr = None
        self.xonxoff = None
        self.rts = None
        self.dtr = None

    def open(self):
        self.ops.append((
            "open",
            self.port,
            self.baudrate,
            self.timeout,
            self.write_timeout,
            self.rtscts,
            self.dsrdtr,
            self.xonxoff,
            self.rts,
            self.dtr,
        ))

    def setDTR(self, value):
        self.ops.append(("dtr", value))

    def setRTS(self, value):
        self.ops.append(("rts", value))


class Ws63AutoBurnTest(unittest.TestCase):
    def test_open_serial_for_burn_preloads_control_lines_before_open_without_post_assert(self):
        created = []

        def serial_factory():
            ser = FakeOpenSerial()
            created.append(ser)
            return ser

        with mock.patch.object(ws63_auto_burn.serial, "Serial", serial_factory):
            ser = ws63_auto_burn.open_serial_for_burn(
                "COM14",
                115200,
                timeout=1,
                idle_rts=False,
                idle_dtr=False,
            )

        self.assertIs(ser, created[0])
        self.assertEqual(
            ser.ops,
            [
                ("open", "COM14", 115200, 1, None, False, False, False, False, False),
            ],
        )

    def test_open_serial_for_burn_can_assert_control_lines_after_open(self):
        created = []

        def serial_factory():
            ser = FakeOpenSerial()
            created.append(ser)
            return ser

        with mock.patch.object(ws63_auto_burn.serial, "Serial", serial_factory):
            ser = ws63_auto_burn.open_serial_for_burn(
                "COM14",
                115200,
                timeout=1,
                idle_rts=False,
                idle_dtr=False,
                assert_after_open=True,
            )

        self.assertIs(ser, created[0])
        self.assertEqual(
            ser.ops,
            [
                ("open", "COM14", 115200, 1, None, False, False, False, False, False),
                ("rts", False),
                ("dtr", False),
            ],
        )

    def test_open_serial_for_burn_sets_requested_write_timeout(self):
        created = []

        def serial_factory():
            ser = FakeOpenSerial()
            created.append(ser)
            return ser

        with mock.patch.object(ws63_auto_burn.serial, "Serial", serial_factory):
            ser = ws63_auto_burn.open_serial_for_burn(
                "COM14",
                115200,
                timeout=1,
                write_timeout=0,
                idle_rts=False,
                idle_dtr=False,
            )

        self.assertIs(ser, created[0])
        self.assertEqual(
            ser.ops,
            [
                ("open", "COM14", 115200, 1, 0, False, False, False, False, False),
            ],
        )

    def test_restore_idle_control_lines_sets_rts_and_dtr(self):
        ser = FakeSerial()

        ws63_auto_burn.restore_idle_control_lines(
            ser,
            idle_rts=True,
            idle_dtr=False,
            log_fn=lambda _message: None,
        )

        self.assertEqual(ser.ops, [("rts", True), ("dtr", False)])

    def test_perform_control_sequence_sends_requested_steps(self):
        ser = FakeSerial()
        sleeps = []

        ws63_auto_burn.perform_control_sequence(
            ser,
            ws63_auto_burn.parse_control_sequence("rts=1:0.02;rts=0,dtr=1:0.03"),
            label="Open control",
            sleep_fn=sleeps.append,
            log_fn=lambda _message: None,
        )

        self.assertEqual(ser.ops, [("rts", True), ("rts", False), ("dtr", True)])
        self.assertEqual(sleeps, [0.02, 0.03])

    def test_control_line_guard_blocks_late_rts_dtr_changes(self):
        class GuardedSerial:
            rts = False
            dtr = False
            baudrate = 115200
            cts = False
            dsr = False
            cd = False
            ri = False
            in_waiting = 0
            out_waiting = 0

            def setRTS(self, value):
                self.rts = value

            def setDTR(self, value):
                self.dtr = value

        guarded = ws63_auto_burn.install_control_line_guard(GuardedSerial(), context="test")

        with self.assertRaises(RuntimeError):
            guarded.setRTS(True)
        with self.assertRaises(RuntimeError):
            guarded.dtr = True

    def test_derive_hold_control_state_keeps_final_reset_sequence_state(self):
        config = ws63_auto_burn.ResetConfig(
            command="",
            fallback_command="",
            sequence=ws63_auto_burn.parse_control_sequence("rts=0:0.25;rts=1:0.5"),
        )

        self.assertEqual(
            ws63_auto_burn.derive_hold_control_state(False, True, config),
            (True, True),
        )

    def test_derive_hold_control_state_uses_idle_state_without_control_sequence(self):
        config = ws63_auto_burn.ResetConfig(command="reboot", fallback_command="", sequence=())

        self.assertEqual(
            ws63_auto_burn.derive_hold_control_state(False, True, config),
            (False, True),
        )

    def test_perform_auto_reset_sends_reboot_then_control_sequence(self):
        ser = FakeSerial()
        sleeps = []
        config = ws63_auto_burn.ResetConfig(
            command="reboot",
            fallback_command="",
            command_delay_s=0.25,
            command_retries=1,
            retry_gap_s=0.0,
            sequence=ws63_auto_burn.parse_control_sequence("rts=0,dtr=0:0.1;rts=0,dtr=1:0.2"),
        )

        ws63_auto_burn.perform_auto_reset(ser, config, sleep_fn=sleeps.append, log_fn=lambda message: None)

        self.assertEqual(
            ser.ops,
            [
                ("write", b"\r\n"),
                ("flush",),
                ("write", b"reboot\r\n"),
                ("flush",),
                ("rts", False),
                ("dtr", False),
                ("rts", False),
                ("dtr", True),
                ("reset_input_buffer",),
            ],
        )
        self.assertEqual(sleeps, [0.05, 0.25, 0.1, 0.2])

    def test_perform_auto_reset_software_only_retries_commands(self):
        ser = FakeSerial()
        sleeps = []
        config = ws63_auto_burn.ResetConfig(
            command="reboot",
            fallback_command="reset",
            command_delay_s=0.1,
            command_retries=2,
            retry_gap_s=0.05,
            sequence=(),
        )

        ws63_auto_burn.perform_auto_reset(ser, config, sleep_fn=sleeps.append, log_fn=lambda message: None)

        self.assertEqual(
            ser.ops,
            [
                ("write", b"\r\n"),
                ("flush",),
                ("write", b"reboot\r\n"),
                ("flush",),
                ("write", b"\r\n"),
                ("flush",),
                ("write", b"reset\r\n"),
                ("flush",),
                ("write", b"\r\n"),
                ("flush",),
                ("write", b"reboot\r\n"),
                ("flush",),
                ("write", b"\r\n"),
                ("flush",),
                ("write", b"reset\r\n"),
                ("flush",),
                ("reset_input_buffer",),
            ],
        )
        self.assertEqual(sleeps, [0.05, 0.1, 0.05, 0.1, 0.05, 0.05, 0.1, 0.05, 0.1])

    def test_perform_auto_reset_can_disable_blank_preamble(self):
        ser = FakeSerial()
        sleeps = []
        config = ws63_auto_burn.ResetConfig(
            command="reboot",
            fallback_command="reset",
            compatibility_command="AT+RST",
            command_delay_s=0.1,
            command_retries=1,
            retry_gap_s=0.0,
            send_preamble=False,
            sequence=(),
        )

        ws63_auto_burn.perform_auto_reset(ser, config, sleep_fn=sleeps.append, log_fn=lambda message: None)

        self.assertEqual(
            ser.ops,
            [
                ("write", b"reboot\r\n"),
                ("flush",),
                ("write", b"reset\r\n"),
                ("flush",),
                ("write", b"AT+RST\r\n"),
                ("flush",),
                ("reset_input_buffer",),
            ],
        )
        self.assertEqual(sleeps, [0.1, 0.1, 0.1])

    def test_parse_control_sequence_rejects_unknown_signal(self):
        with self.assertRaises(ValueError):
            ws63_auto_burn.parse_control_sequence("boot=1:0.1")

    def test_show_mode_does_not_require_port(self):
        parser = ws63_auto_burn.build_arg_parser()

        args = parser.parse_args(["-s", "firmware.fwpkg"])

        self.assertTrue(args.show)
        self.assertIsNone(args.port)

    def test_parser_supports_software_reset_only_mode(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args(["-p", "/dev/null", "--software-reset-only", "firmware.fwpkg"])
        self.assertTrue(args.software_reset_only)

    def test_parser_supports_idle_control_line_states(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args([
            "-p", "/dev/null",
            "--idle-rts", "1",
            "--idle-dtr", "0",
            "firmware.fwpkg",
        ])
        self.assertTrue(args.idle_rts)
        self.assertFalse(args.idle_dtr)

    def test_parser_supports_ymodem_packet_size(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args(["-p", "/dev/null", "--ymodem-packet-size", "128", "firmware.fwpkg"])
        self.assertEqual(args.ymodem_packet_size, 128)

    def test_parser_supports_ymodem_transfer_retries(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args(["-p", "/dev/null", "--ymodem-transfer-retries", "3", "firmware.fwpkg"])
        self.assertEqual(args.ymodem_transfer_retries, 3)

    def test_parser_supports_serial_write_limiter(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args([
            "-p", "/dev/null",
            "--serial-write-chunk-size", "16",
            "--serial-write-gap", "0.001",
            "firmware.fwpkg",
        ])
        self.assertEqual(args.serial_write_chunk_size, 16)
        self.assertEqual(args.serial_write_gap, 0.001)

    def test_serial_write_limiter_splits_writes(self):
        class FakeWriteSerial:
            def __init__(self):
                self.writes = []

            def write(self, data):
                self.writes.append(bytes(data))
                return len(data)

        ser = FakeWriteSerial()
        sleeps = []
        ws63_auto_burn.install_serial_write_limiter(ser, 3, 0.1, sleep_fn=sleeps.append)

        written = ser.write(b"abcdefg")

        self.assertEqual(written, 7)
        self.assertEqual(ser.writes, [b"abc", b"def", b"g"])
        self.assertEqual(sleeps, [0.1, 0.1])

    def test_nonblocking_drain_writer_preserves_full_write(self):
        class FakeDrainSerial:
            def __init__(self):
                self.writes = []
                self.out_waiting_values = [2, 1, 0]

            def write(self, data):
                self.writes.append(bytes(data))
                return len(data)

            @property
            def out_waiting(self):
                return self.out_waiting_values.pop(0)

        ser = FakeDrainSerial()
        sleeps = []
        ws63_auto_burn.install_nonblocking_drain_writer(
            ser,
            drain_timeout_s=1.0,
            poll_s=0.1,
            sleep_fn=sleeps.append,
        )

        written = ser.write(b"abcdefg")

        self.assertEqual(written, 7)
        self.assertEqual(ser.writes, [b"abcdefg"])
        self.assertEqual(sleeps, [0.1, 0.1])

    def test_wait_for_ymodem_receiver_ignores_cached_ready_until_fresh_c(self):
        if ws63_auto_burn.vendor_pymodem is None:
            self.skipTest("xf_burn_tools is not installed")

        class FakeNoInputSerial:
            in_waiting = 0

            def read(self, _size):
                return b""

        with mock.patch.object(ws63_auto_burn.time, "time", side_effect=[0.0, 6.0]):
            self.assertFalse(
                ws63_auto_burn.wait_for_ymodem_receiver(
                    FakeNoInputSerial(),
                    receiver_ready=True,
                )
            )

    def test_wait_for_ymodem_receiver_accepts_fresh_c(self):
        if ws63_auto_burn.vendor_pymodem is None:
            self.skipTest("xf_burn_tools is not installed")

        class FakeReadySerial:
            def __init__(self):
                self.reads = [bytes([ws63_auto_burn.vendor_pymodem.C])]

            @property
            def in_waiting(self):
                return len(self.reads)

            def read(self, _size):
                return self.reads.pop(0)

        self.assertTrue(
            ws63_auto_burn.wait_for_ymodem_receiver(
                FakeReadySerial(),
                receiver_ready=True,
            )
        )

    def test_wait_for_ymodem_receiver_reuses_cached_ready_only_when_explicit(self):
        if ws63_auto_burn.vendor_pymodem is None:
            self.skipTest("xf_burn_tools is not installed")

        class FakeNoInputSerial:
            in_waiting = 0

            def read(self, _size):
                return b""

        self.assertTrue(
            ws63_auto_burn.wait_for_ymodem_receiver(
                FakeNoInputSerial(),
                receiver_ready=True,
                reuse_cached_ready=True,
            )
        )

    def test_drain_serial_input_discards_rom_handshake_tail(self):
        tail = (
            b"\xa5\x5a\xd5\xda"
            b"Unsupport CMD:0xF0\r\n"
            b"\xef\xbe\xad\xde\x0c\x00\xe1\x1e"
            b"\xa5\x5a\xd5\xda"
        )

        class FakeTailSerial:
            def __init__(self):
                self.buffer = bytearray(tail)
                self.read_sizes = []

            @property
            def in_waiting(self):
                return len(self.buffer)

            def read(self, size):
                self.read_sizes.append(size)
                chunk = bytes(self.buffer[:size])
                del self.buffer[:size]
                return chunk

        ser = FakeTailSerial()

        drained = ws63_auto_burn.drain_serial_input(ser)

        self.assertEqual(drained, len(tail))
        self.assertEqual(ser.in_waiting, 0)
        self.assertEqual(ser.read_sizes, [len(tail)])

    def test_parser_supports_serial_write_mode_and_control_assert(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args([
            "-p", "/dev/null",
            "--serial-write-mode", "nonblocking-drain",
            "--serial-write-drain-timeout", "3.5",
            "--assert-control-after-open",
            "--open-control-sequence", "rts=1:0.02;rts=0:0.02",
            "firmware.fwpkg",
        ])

        self.assertEqual(args.serial_write_mode, "nonblocking-drain")
        self.assertEqual(args.serial_write_drain_timeout, 3.5)
        self.assertTrue(args.assert_control_after_open)
        self.assertEqual(args.open_control_sequence, "rts=1:0.02;rts=0:0.02")

    def test_parser_supports_flash_attempts(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args(["-p", "/dev/null", "--flash-attempts", "2", "firmware.fwpkg"])

        self.assertEqual(args.flash_attempts, 2)

    def test_parser_supports_rom_preflight(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args([
            "-p", "/dev/null",
            "--skip-reset-if-rom-active",
            "--rom-preflight-timeout", "1.5",
            "firmware.fwpkg",
        ])

        self.assertTrue(args.skip_reset_if_rom_active)
        self.assertEqual(args.rom_preflight_timeout, 1.5)

    def test_parser_supports_handshake_tail_recovery(self):
        parser = ws63_auto_burn.build_arg_parser()
        args = parser.parse_args([
            "-p", "/dev/null",
            "--reuse-handshake-receiver-c",
            "--drain-handshake-tail",
            "firmware.fwpkg",
        ])

        self.assertTrue(args.reuse_handshake_receiver_c)
        self.assertTrue(args.drain_handshake_tail)

    def test_firmware_version_guard_checks_package_bytes(self):
        path = None
        try:
            with tempfile.NamedTemporaryFile(delete=False) as f:
                path = f.name
                f.write(b"boot data v4.4.64 app data")
                f.flush()

            self.assertTrue(ws63_auto_burn.firmware_contains_version(path, "v4.4.64"))
            self.assertFalse(ws63_auto_burn.firmware_contains_version(path, "v4.4.60"))
            self.assertTrue(ws63_auto_burn.firmware_contains_version(path, ""))
        finally:
            if path is not None:
                os.unlink(path)

    def test_main_refuses_stale_firmware_before_flash(self):
        path = None
        try:
            with tempfile.NamedTemporaryFile(delete=False) as f:
                path = f.name
                f.write(b"boot data v4.4.37 app data")
                f.flush()

            ret = ws63_auto_burn.main(["-p", "/dev/null", "--no-auto-reset", "--expected-version", "v4.4.64", path])
        finally:
            if path is not None:
                os.unlink(path)

        self.assertEqual(ret, 3)

    def test_main_returns_nonzero_when_flash_fails(self):
        seen = {}

        class FailingBurner:
            def __init__(self, *args, **kwargs):
                seen["args"] = args
                seen["kwargs"] = kwargs

            def flash(self, _firmware):
                return False

        with mock.patch.object(ws63_auto_burn, "HAVE_XF_BURN_TOOLS", True), \
                mock.patch.object(ws63_auto_burn, "AutoResetWs63BurnTools", FailingBurner):
            ret = ws63_auto_burn.main([
                "-p", "/dev/null",
                "--software-reset-only",
                "--legacy-reset-order",
                "--no-reset-preamble",
                "--expected-version", "",
                "firmware.fwpkg",
            ])

        self.assertNotEqual(ret, 0)
        self.assertTrue(seen["kwargs"]["legacy_reset_order"])
        self.assertEqual(seen["kwargs"]["ymodem_packet_size"], 1024)
        self.assertEqual(seen["kwargs"]["ymodem_transfer_retries"], 1)
        self.assertEqual(seen["kwargs"]["serial_write_chunk_size"], 0)
        self.assertEqual(seen["kwargs"]["serial_write_gap_s"], 0.0)
        self.assertEqual(seen["kwargs"]["serial_write_mode"], "nonblocking-drain")
        self.assertEqual(seen["kwargs"]["serial_write_drain_timeout_s"], 3.0)
        self.assertEqual(seen["kwargs"]["serial_write_post_gap_s"], 0.0)
        self.assertFalse(seen["kwargs"]["idle_rts"])
        self.assertFalse(seen["kwargs"]["idle_dtr"])
        self.assertFalse(seen["kwargs"]["assert_control_after_open"])
        self.assertEqual(seen["kwargs"]["open_control_sequence"], [])
        self.assertFalse(seen["kwargs"]["skip_reset_if_rom_active"])
        self.assertEqual(seen["kwargs"]["rom_preflight_timeout_s"], 1.0)
        self.assertFalse(seen["args"][2].send_preamble)

    def test_main_passes_serial_tuning_to_burner(self):
        seen = {}

        class FailingBurner:
            def __init__(self, *args, **kwargs):
                seen["args"] = args
                seen["kwargs"] = kwargs

            def flash(self, _firmware):
                return False

        with mock.patch.object(ws63_auto_burn, "HAVE_XF_BURN_TOOLS", True), \
                mock.patch.object(ws63_auto_burn, "AutoResetWs63BurnTools", FailingBurner):
            ws63_auto_burn.main([
                "-p", "/dev/null",
                "--no-auto-reset",
                "--expected-version", "",
                "--ymodem-packet-size", "128",
                "--ymodem-transfer-retries", "3",
                "--serial-write-chunk-size", "16",
                "--serial-write-gap", "0.001",
                "--serial-write-mode", "nonblocking-drain",
                "--serial-write-drain-timeout", "3.5",
                "--serial-write-post-gap", "0.02",
                "--idle-rts", "1",
                "--idle-dtr", "0",
                "--assert-control-after-open",
                "--open-control-sequence", "rts=1:0.02;rts=0:0.02",
                "--skip-reset-if-rom-active",
                "--rom-preflight-timeout", "1.5",
                "--reuse-handshake-receiver-c",
                "--drain-handshake-tail",
                "firmware.fwpkg",
            ])

        self.assertEqual(seen["kwargs"]["ymodem_packet_size"], 128)
        self.assertEqual(seen["kwargs"]["ymodem_transfer_retries"], 3)
        self.assertEqual(seen["kwargs"]["serial_write_chunk_size"], 16)
        self.assertEqual(seen["kwargs"]["serial_write_gap_s"], 0.001)
        self.assertEqual(seen["kwargs"]["serial_write_mode"], "nonblocking-drain")
        self.assertEqual(seen["kwargs"]["serial_write_drain_timeout_s"], 3.5)
        self.assertEqual(seen["kwargs"]["serial_write_post_gap_s"], 0.02)
        self.assertTrue(seen["kwargs"]["idle_rts"])
        self.assertFalse(seen["kwargs"]["idle_dtr"])
        self.assertTrue(seen["kwargs"]["assert_control_after_open"])
        self.assertTrue(seen["kwargs"]["skip_reset_if_rom_active"])
        self.assertEqual(seen["kwargs"]["rom_preflight_timeout_s"], 1.5)
        self.assertTrue(seen["kwargs"]["reuse_handshake_receiver_c"])
        self.assertTrue(seen["kwargs"]["drain_handshake_tail"])
        self.assertEqual(
            seen["kwargs"]["open_control_sequence"],
            [
                ws63_auto_burn.ControlStep(rts=True, delay_s=0.02),
                ws63_auto_burn.ControlStep(rts=False, delay_s=0.02),
            ],
        )

    def test_main_retries_complete_flash_sessions(self):
        attempts = []

        class RetryingBurner:
            def __init__(self, *args, **kwargs):
                self.ser = None

            def flash(self, _firmware):
                attempts.append("flash")
                return len(attempts) == 2

        with mock.patch.object(ws63_auto_burn, "HAVE_XF_BURN_TOOLS", True), \
                mock.patch.object(ws63_auto_burn, "AutoResetWs63BurnTools", RetryingBurner), \
                mock.patch.object(ws63_auto_burn.time, "sleep", lambda _seconds: None):
            ret = ws63_auto_burn.main([
                "-p", "/dev/null",
                "--no-auto-reset",
                "--expected-version", "",
                "--flash-attempts", "2",
                "firmware.fwpkg",
            ])

        self.assertEqual(ret, 0)
        self.assertEqual(attempts, ["flash", "flash"])

    def test_main_defaults_to_current_expected_firmware_version(self):
        parser = ws63_auto_burn.build_arg_parser()

        args = parser.parse_args(["-p", "/dev/null", "--software-reset-only", "firmware.fwpkg"])

        self.assertEqual(args.expected_version, "v4.5.64-minimal")

    def test_main_show_mode_does_not_apply_version_guard(self):
        class FakeFwpkg:
            def __init__(self, firmware_file):
                self.firmware_file = firmware_file

            def show(self):
                return None

        with tempfile.NamedTemporaryFile(delete=False) as f:
            path = f.name
            f.write(b"boot data v4.4.64 app data")
            f.flush()
        try:
            with mock.patch.object(ws63_auto_burn, "HAVE_XF_BURN_TOOLS", True), \
                    mock.patch.object(ws63_auto_burn, "Fwpkg", FakeFwpkg):
                ret = ws63_auto_burn.main(["--show", "--expected-version", "v4.4.64", path])
        finally:
            os.unlink(path)

        self.assertEqual(ret, 0)


if __name__ == "__main__":
    unittest.main()
