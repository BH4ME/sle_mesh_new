import pathlib
import subprocess
import sys
import unittest

from tools.sle_team_python_sim import SimulationConfig, simulate_one_run


class OneVsTwentySimulationTest(unittest.TestCase):
    def test_end_to_end_join_report_failover_recover(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=6,
            recover_relay_at_tick=10,
            ticks_total=14,
        )
        result = simulate_one_run(cfg)

        self.assertEqual(result.discovered_members, 20)
        self.assertEqual(result.approved_members, 20)
        self.assertGreaterEqual(result.report_success_before_failover, 20 * 2)
        self.assertGreaterEqual(result.report_success_during_failover, (20 - 1) * 2)
        self.assertGreaterEqual(result.report_success_after_recover, 20 * 2)
        self.assertGreaterEqual(result.route_reparent_total, 1)
        self.assertGreaterEqual(result.relay_reselection_total, 1)

    def test_stress_runs_are_stable(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=5,
            recover_relay_at_tick=9,
            ticks_total=12,
        )
        for _ in range(10):
            result = simulate_one_run(cfg)
            self.assertEqual(result.discovered_members, 20)
            self.assertEqual(result.approved_members, 20)
            self.assertGreater(result.total_report_success, 0)

    def test_packet_loss_and_jitter_take_effect(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=6,
            recover_relay_at_tick=10,
            ticks_total=14,
            packet_loss_rate=0.25,
            jitter_min_ms=0,
            jitter_max_ms=8,
        )
        result = simulate_one_run(cfg)
        self.assertEqual(result.discovered_members, 20)
        self.assertEqual(result.approved_members, 20)
        self.assertGreater(result.report_dropped + result.report_delayed, 0)

    def test_batch_relay_failover_is_handled(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=0,  # disable single event by choosing out-of-range
            recover_relay_at_tick=20,
            ticks_total=14,
            batch_fail_relay_count=2,
            batch_fail_relay_ticks=[5, 9],
        )
        result = simulate_one_run(cfg)
        self.assertEqual(result.discovered_members, 20)
        self.assertEqual(result.approved_members, 20)
        self.assertGreaterEqual(result.batch_fail_events, 1)
        self.assertGreaterEqual(result.relay_reselection_total, 1)

    def test_thirty_node_relay_failover_reparents_without_lost_parent(self) -> None:
        cfg = SimulationConfig(
            member_count=30,
            direct_connection_cap=8,
            fail_relay_at_tick=6,
            recover_relay_at_tick=10,
            ticks_total=16,
            relay_target=3,
            batch_fail_relay_count=2,
            batch_fail_relay_ticks=[8, 12],
            jitter_min_ms=0,
            jitter_max_ms=80,
            seed=20260604,
        )
        result = simulate_one_run(cfg)

        self.assertEqual(result.discovered_members, 30)
        self.assertEqual(result.approved_members, 30)
        self.assertGreater(result.report_success_before_failover, 0)
        self.assertGreater(result.report_success_during_failover, 0)
        self.assertGreater(result.report_success_after_recover, 0)
        self.assertGreaterEqual(result.batch_fail_events, 2)
        self.assertGreaterEqual(result.route_reparent_total, 1)
        self.assertGreaterEqual(result.relay_reselection_total, 3)
        self.assertEqual(result.report_lost_by_parent_down, 0)

    def test_high_jitter_delays_reports_without_zeroing_all_success(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=6,
            recover_relay_at_tick=10,
            ticks_total=18,
            packet_loss_rate=0.2,
            jitter_min_ms=10,
            jitter_max_ms=120,
            batch_fail_relay_count=1,
            batch_fail_relay_ticks=[6],
        )
        result = simulate_one_run(cfg)
        self.assertEqual(result.discovered_members, 20)
        self.assertEqual(result.approved_members, 20)
        self.assertGreater(result.report_delayed, 0)
        self.assertGreater(result.total_report_success, 0)

    def test_delayed_reports_are_counted_by_send_tick(self) -> None:
        cfg = SimulationConfig(
            member_count=20,
            direct_connection_cap=8,
            fail_relay_at_tick=2,
            recover_relay_at_tick=3,
            ticks_total=3,
            packet_loss_rate=0.0,
            jitter_min_ms=10,
            jitter_max_ms=10,
        )
        result = simulate_one_run(cfg)
        self.assertEqual(result.discovered_members, 20)
        self.assertEqual(result.approved_members, 20)
        self.assertGreater(result.report_success_before_failover, 0)
        self.assertGreater(result.report_success_during_failover, 0)
        self.assertGreater(result.report_success_after_recover, 0)

    def test_cli_passes_when_no_failover_event_is_configured(self) -> None:
        sim_path = pathlib.Path(__file__).resolve().parent / "sle_team_python_sim.py"
        proc = subprocess.run(
            [
                sys.executable,
                str(sim_path),
                "--members",
                "20",
                "--relay-fail-tick",
                "20",
                "--relay-recover-tick",
                "20",
                "--ticks",
                "10",
                "--batch-fail-relay-count",
                "0",
                "--stress",
                "1",
            ],
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(
            proc.returncode,
            0,
            msg=f"unexpected exit={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
