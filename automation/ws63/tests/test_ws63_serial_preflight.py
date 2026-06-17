import argparse
import io
import unittest
from contextlib import redirect_stdout

from automation.ws63.tools import ws63_serial_preflight as sp


class Ws63SerialPreflightTest(unittest.TestCase):
    def test_select_board_ports_excludes_system_com1(self):
        ports = [
            sp.PortInfo("COM1", "通信端口 (COM1)", "ACPI\\PNP0501\\0"),
            sp.PortInfo("COM13", "USB-SERIAL CH340 (COM13)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM16", "USB-SERIAL CH340 (COM16)", "USB VID:PID=1A86:7523"),
        ]

        selected = sp.select_board_ports(ports)

        self.assertEqual([port.device for port in selected], ["COM13", "COM16"])

    def test_validate_requested_ports_requires_three_unique_board_ports(self):
        available = [
            sp.PortInfo("COM13", "USB-SERIAL CH340 (COM13)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM16", "USB-SERIAL CH340 (COM16)", "USB VID:PID=1A86:7523"),
        ]

        ok, errors = sp.validate_requested_ports(available, ["COM16", "COM13"])

        self.assertFalse(ok)
        self.assertIn("relay-cycle requires three ports: leader,relay,child", errors)

    def test_validate_requested_ports_rejects_non_board_port(self):
        available = [
            sp.PortInfo("COM1", "通信端口 (COM1)", "ACPI\\PNP0501\\0"),
            sp.PortInfo("COM13", "USB-SERIAL CH340 (COM13)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM16", "USB-SERIAL CH340 (COM16)", "USB VID:PID=1A86:7523"),
        ]

        ok, errors = sp.validate_requested_ports(available, ["COM16", "COM13", "COM1"])

        self.assertFalse(ok)
        self.assertIn("port does not look like a WS63 board UART: COM1", errors)

    def test_relay_cycle_run_passes_with_three_detected_board_ports(self):
        available = [
            sp.PortInfo("COM16", "USB-SERIAL CH340 (COM16)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM13", "USB-SERIAL CH340 (COM13)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM17", "JLink CDC UART Port (COM17)", "USB VID:PID=1366:1069"),
        ]
        original = sp.list_serial_ports
        sp.list_serial_ports = lambda: available
        out = io.StringIO()
        try:
            with redirect_stdout(out):
                ret = sp.run(argparse.Namespace(mode="relay-cycle", ports="", exclude="COM1"))
        finally:
            sp.list_serial_ports = original

        self.assertEqual(ret, 0)
        self.assertIn("PASS: found at least 3 board ports", out.getvalue())
        self.assertIn("--ports COM16,COM13,COM17", out.getvalue())

    def test_relay_cycle_run_fails_with_only_two_detected_board_ports(self):
        available = [
            sp.PortInfo("COM1", "通信端口 (COM1)", "ACPI\\PNP0501\\0"),
            sp.PortInfo("COM13", "USB-SERIAL CH340 (COM13)", "USB VID:PID=1A86:7523"),
            sp.PortInfo("COM16", "USB-SERIAL CH340 (COM16)", "USB VID:PID=1A86:7523"),
        ]
        original = sp.list_serial_ports
        sp.list_serial_ports = lambda: available
        out = io.StringIO()
        try:
            with redirect_stdout(out):
                ret = sp.run(argparse.Namespace(mode="relay-cycle", ports="", exclude="COM1"))
        finally:
            sp.list_serial_ports = original

        self.assertEqual(ret, 1)
        self.assertIn("need 3 board ports for relay-cycle, found 2", out.getvalue())


if __name__ == "__main__":
    unittest.main()
