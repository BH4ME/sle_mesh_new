import unittest
from unittest import mock

from automation.ws63.tools import ws63_link_cycle_test as lc
from automation.ws63.tools import ws63_natural_relay_soak_double_drop_test as nd


class NaturalRelaySoakDoubleDropUnitTest(unittest.TestCase):
    def test_initial_topology_ready_falls_back_when_route_gate_does_not_refresh(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader]
        records = {
            164: {"online": 1, "relay": 1},
            182: {"online": 1, "relay": 0},
            37: {"online": 1, "relay": 0},
        }

        with mock.patch.object(
            nd.fb,
            "_wait_stable_final_topology",
            side_effect=RuntimeError("timeout waiting for stable final topology: stable=0/4 direct_cap=1 route_ok=False last={}"),
        ) as wait_topology, mock.patch.object(
            nd.fb,
            "_wait_member_records",
            return_value=records,
        ) as wait_members, mock.patch.object(
            nd,
            "_wait_relay_count",
            return_value=records,
        ) as wait_relay_count:
            result = nd._wait_initial_topology_ready(
                leader,
                peers,
                leader_id=170,
                member_ids=[164, 182, 37],
                direct_cap=1,
                relay_target=1,
            timeout_s=1.0,
            poll_s=0.1,
            log_start=0,
            stable_polls=2,
        )

        self.assertIs(result, records)
        wait_topology.assert_called_once()
        wait_members.assert_called_once()
        wait_relay_count.assert_called_once()

    def test_wait_soak_allows_missing_route_metrics_when_member_and_relay_state_are_healthy(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader]
        records = {
            164: {"online": 1, "relay": 1},
            182: {"online": 1, "relay": 0},
            37: {"online": 1, "relay": 0},
        }

        with mock.patch.object(
            nd.fb,
            "_query_records_once",
            return_value=records,
        ) as query_records, mock.patch.object(
            nd.fb,
            "_relay_ids_from_records",
            return_value=[164],
        ) as relay_ids, mock.patch.object(
            nd.nr,
            "_assert_no_route_regressions_scoped",
        ) as assert_no_regressions, mock.patch.object(
            nd.time,
            "sleep",
            return_value=None,
        ), mock.patch.object(
            nd.time,
            "time",
            side_effect=[100.0, 100.0, 100.1, 100.2, 100.3, 101.1, 101.2, 101.3],
        ):
            result = nd._wait_soak(
                leader,
                peers,
                leader_id=170,
                member_ids=[164, 182, 37],
                expected_relay_count=1,
                duration_s=1.0,
                poll_s=0.1,
                report_interval_s=10.0,
                max_bad_polls=3,
                log_start=0,
                regression_log_start_by_name={"leader": 0},
                label="pre-drop",
            )

        self.assertEqual(result["poll_count"], 1)
        self.assertEqual(result["bad_samples"], [])
        self.assertEqual(result["route_metrics_missing_polls"], 1)
        self.assertIsNone(result["final_route_metrics"])
        query_records.assert_called()
        relay_ids.assert_called()
        assert_no_regressions.assert_called_once()

    def test_wait_soak_fails_when_parent_map_moves_during_stable_window(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader]
        moved_records = {
            164: {"online": 1, "relay": 1, "parent_id": 170},
            182: {"online": 1, "relay": 0, "parent_id": 37},
            37: {"online": 1, "relay": 0, "parent_id": 164},
        }

        with mock.patch.object(
            nd.fb,
            "_query_records_once",
            return_value=moved_records,
        ), mock.patch.object(
            nd.fb,
            "_relay_ids_from_records",
            return_value=[164],
        ), mock.patch.object(
            nd.nr,
            "_assert_no_route_regressions_scoped",
        ), mock.patch.object(
            nd.time,
            "sleep",
            return_value=None,
        ), mock.patch.object(
            nd.time,
            "time",
            side_effect=[100.0, 100.0, 100.1, 100.2, 100.3, 100.4, 100.5, 100.6, 100.7, 101.1, 101.2],
        ):
            with self.assertRaisesRegex(RuntimeError, "parent_map="):
                nd._wait_soak(
                    leader,
                    peers,
                    leader_id=170,
                    member_ids=[164, 182, 37],
                    expected_relay_count=1,
                    duration_s=1.0,
                    poll_s=0.1,
                    report_interval_s=10.0,
                    max_bad_polls=1,
                    log_start=0,
                    regression_log_start_by_name={"leader": 0},
                    label="stable",
                    expected_parent_by_member={164: 170, 182: 164, 37: 164},
                    expected_relay_ids=[164],
                )


if __name__ == "__main__":
    unittest.main()
