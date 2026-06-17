import unittest
from pathlib import Path


EXPECTED_VERSION = "v4.5.38-minimal"


class FiveBoardMemberLossContractTest(unittest.TestCase):
    REPO_ROOT = Path(__file__).resolve().parents[3]

    def _read(self, rel_path: str) -> str:
        return (self.REPO_ROOT / rel_path).read_text(encoding="utf-8")

    def test_five_board_member_loss_uses_natural_enrollment_and_current_fw(self):
        source = self._read("automation/ws63/tools/ws63_five_board_member_loss_test.py")

        self.assertIn(f'parser.add_argument("--expected-fw", default="{EXPECTED_VERSION}")', source)
        self.assertIn('summary["enrollment_mode"] = "allow_all_no_pairing_window"', source)
        self.assertIn("nr._wait_natural_enrollment(", source)
        self.assertIn("nr._recover_dropped_member(", source)
        self.assertIn("nr._wait_topology_ready(", source)
        self.assertIn("nr._snapshot_peer_join_state(", source)
        self.assertNotIn('_progress("pairing start")', source)
        self.assertNotIn('command="pairing start"', source)
        self.assertNotIn('command="pairing stop"', source)
        self.assertNotIn("fb._approve_member(", source)

    def test_legacy_relay_member_ports_is_documented_as_ignored(self):
        source = self._read("automation/ws63/tools/ws63_five_board_member_loss_test.py")

        self.assertIn("legacy argument kept for compatibility; ignored under natural enrollment", source)
        self.assertIn("legacy relay_member_ports ignored under natural enrollment", source)


if __name__ == "__main__":
    unittest.main()
