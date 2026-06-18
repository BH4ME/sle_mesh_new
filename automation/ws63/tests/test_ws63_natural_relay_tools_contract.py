import unittest
from pathlib import Path

from automation.ws63.tools import ws63_multi_board_relay_recovery_test as mb


EXPECTED_VERSION = "v4.5.64-minimal"


class NaturalRelayToolsContractTest(unittest.TestCase):
    REPO_ROOT = Path(__file__).resolve().parents[3]

    def _read(self, rel_path: str) -> str:
        return (self.REPO_ROOT / rel_path).read_text(encoding="utf-8")

    def test_flash_all_ch340_wrapper_defaults_to_current_version(self):
        script = self._read("scripts/flash/ws63_flash_all_ch340_retry_once.ps1")

        self.assertIn(f'[string]$ExpectedVersion = "{EXPECTED_VERSION}"', script)

    def test_natural_relay_leave_recovery_requires_expected_fw_and_old_relay_rejoin_rule(self):
        source = self._read("automation/ws63/tools/ws63_natural_relay_leave_recovery_test.py")

        self.assertIn(f'parser.add_argument("--expected-fw", default="{EXPECTED_VERSION}")', source)
        self.assertIn("firmware mismatch: expected", source)
        self.assertIn("_assert_old_relay_rejoins_as_child_or_member", source)
        self.assertIn('summary["enrollment_mode"] = "allow_all_no_pairing_window"', source)
        self.assertNotIn('_progress("pairing start")', source)
        self.assertNotIn('command="pairing start"', source)
        self.assertNotIn('command="pairing stop"', source)
        self.assertIn("old relay reclaimed relay role unexpectedly", source)
        self.assertIn("old relay rejoined direct to leader while replacement relay exists", source)

    def test_multi_board_relay_recovery_requires_expected_fw(self):
        source = self._read("automation/ws63/tools/ws63_multi_board_relay_recovery_test.py")

        self.assertIn(f'parser.add_argument("--expected-fw", default="{EXPECTED_VERSION}")', source)
        self.assertIn("firmware mismatch: expected", source)
        self.assertIn('summary["enrollment_mode"] = "allow_all_no_pairing_window"', source)
        self.assertNotIn('_progress("pairing start")', source)
        self.assertNotIn('command="pairing start"', source)
        self.assertNotIn('command="pairing stop"', source)
        self.assertIn("nr._wait_natural_enrollment(", source)
        self.assertIn("nr._assert_old_relay_rejoins_as_child_or_member(", source)
        self.assertIn("relay_offline_observed = True", source)
        self.assertIn("except RuntimeError as exc:", source)
        self.assertIn("leader offline event not observed before recovery topology converged", source)
        self.assertIn('"leader_offline_observed": relay_offline_observed', source)
        self.assertIn("relay_restore_log_start = len(relay_peer.log)", source)
        self.assertIn("_assert_runtime_invariants(", source)
        self.assertIn("leader crash/reboot observed", source)
        self.assertIn("direct_count=", source)
        self.assertIn("exceeds direct_cap", source)
        self.assertIn("runtime leader boot marker", source)

    def test_multi_board_relay_recovery_detects_runtime_silent_boot(self):
        leader = mb.lc.Peer(name="leader", port="COMX", baudrate=115200, ser=object())
        leader.log = [
            f"APP|dbg uart init ok.\r\ndevice_main_init: 0!\r\n[team] boot fw={EXPECTED_VERSION}\r\n",
            "[team] configured role=leader\r\n",
            f"APP|dbg uart init ok.\r\ndevice_main_init: 0!\r\n[team] boot fw={EXPECTED_VERSION} route=166\r\n",
        ]

        events = mb._find_leader_crash_events(leader, log_start=1)

        self.assertTrue(any("device_main_init" in event for event in events), events)
        self.assertTrue(any("[team] boot fw=" in event for event in events), events)

    def test_natural_relay_soak_double_drop_requires_current_expected_fw(self):
        source = self._read("automation/ws63/tools/ws63_natural_relay_soak_double_drop_test.py")

        self.assertIn(f'parser.add_argument("--expected-fw", default="{EXPECTED_VERSION}")', source)
        self.assertIn('summary["enrollment_mode"] = "allow_all_no_pairing_window"', source)
        self.assertNotIn('command="pairing start"', source)
        self.assertNotIn('command="pairing stop"', source)


if __name__ == "__main__":
    unittest.main()
