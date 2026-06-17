import unittest

from automation.ws63.tools import ws63_flash_bind_team as fb


class FlashBindTeamUnitTest(unittest.TestCase):
    def test_route_id_from_suffix(self):
        self.assertEqual(fb._route_id_from_suffix(0xC7E9), 53)
        self.assertEqual(fb._route_id_from_suffix(0xC700), 74)
        self.assertEqual(fb._route_id_from_suffix(0x12FF), 52)

    def test_route_id_from_suffix_mixes_high_byte(self):
        self.assertEqual(fb._route_id_from_suffix(0x2277), 158)
        self.assertEqual(fb._route_id_from_suffix(0x2177), 127)
        self.assertNotEqual(fb._route_id_from_suffix(0x2277), fb._route_id_from_suffix(0x2177))

    def test_extract_suffix(self):
        self.assertEqual(fb._extract_suffix("label=LC7E9"), 0xC7E9)
        self.assertEqual(fb._extract_suffix("selfLabel\":\"UE7F1\""), 0xE7F1)
        self.assertEqual(fb._extract_suffix("mac=AA:BB:CC:DD:E7:F1"), 0xE7F1)
        self.assertIsNone(fb._extract_suffix("nothing to parse"))

    def test_parse_ports_dedup_and_strip(self):
        ports = fb._parse_ports(" /dev/tty.A ,/dev/tty.B,/dev/tty.A ")
        self.assertEqual(ports, ["/dev/tty.A", "/dev/tty.B"])

    def test_parse_ports_empty(self):
        with self.assertRaises(ValueError):
            fb._parse_ports(" , , ")


if __name__ == "__main__":
    unittest.main()
