import pathlib
import unittest


class Ws63SystemScriptTest(unittest.TestCase):
    def test_burn_stage_uses_v4_build_script(self):
        repo_root = pathlib.Path(__file__).resolve().parents[3]
        script = (repo_root / "automation/ws63/scripts/ws63_test_system.sh").read_text(encoding="utf-8")

        self.assertIn("scripts/build/ws63_build_v4_ubuntu.sh", script)
        burn_stage = script.split('if [[ "$DO_BURN" == "1" ]]', 1)[1]
        self.assertNotIn("scripts/build/ws63_build_team_ubuntu.sh", burn_stage)

    def test_relay_cycle_stage_uses_three_board_tool(self):
        repo_root = pathlib.Path(__file__).resolve().parents[3]
        script = (repo_root / "automation/ws63/scripts/ws63_test_system.sh").read_text(encoding="utf-8")

        self.assertIn("--with-relay-cycle", script)
        self.assertIn("automation/ws63/tools/ws63_relay_cycle_test.py", script)
        self.assertIn("--leader-port", script)
        self.assertIn("--relay-port", script)
        self.assertIn("--child-port", script)
        self.assertIn("--relay-reboot-command", script)
        self.assertIn("--no-require-child-parent-relay", script)

    def test_relay_cycle_preflight_requires_three_ports(self):
        repo_root = pathlib.Path(__file__).resolve().parents[3]
        script = (repo_root / "automation/ws63/scripts/ws63_test_system.sh").read_text(encoding="utf-8")

        self.assertIn('if [[ "$DO_RELAY_CYCLE" == "1" ]]', script)
        self.assertIn("--with-relay-cycle requires --ports leader,relay,child", script)
        self.assertIn("RELAY_PREFLIGHT_PORTS", script)
        self.assertIn("--with-relay-cycle requires three ports: leader,relay,child", script)
        self.assertIn("automation/ws63/tools/ws63_serial_preflight.py", script)
        self.assertIn("relay-cycle serial preflight failed", script)
        self.assertIn("unit_ws63_serial_preflight", script)


if __name__ == "__main__":
    unittest.main()
