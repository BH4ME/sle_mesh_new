import unittest
from unittest import mock

from automation.ws63.tools import ws63_natural_relay_leave_recovery_test as nr
from automation.ws63.tools import ws63_link_cycle_test as lc


class NaturalRelayLeaveRecoveryUnitTest(unittest.TestCase):
    def test_parser_defaults_to_current_expected_firmware(self):
        parser = nr.build_parser()

        args = parser.parse_args([
            "--leader-port",
            "COM1",
            "--member-ports",
            "COM2",
            "COM3",
            "--log-dir",
            "logs/test",
        ])

        self.assertEqual(args.expected_fw, "v4.5.64-minimal")
        self.assertEqual(args.drop_method, "reboot")

    def test_old_relay_rejoin_rejects_reclaiming_relay_role(self):
        topology = {
            "parent_by_member": {"164": 170, "182": 170, "37": 182},
            "route_next_by_member": {"164": 170, "182": 170, "37": 182},
            "tree_lines": [],
        }
        records = {
            164: {"online": 1, "relay": 1},
            182: {"online": 1, "relay": 1},
            37: {"online": 1, "relay": 0},
        }

        with self.assertRaisesRegex(RuntimeError, "reclaimed relay role unexpectedly"):
            nr._assert_old_relay_rejoins_as_child_or_member(
                topology,
                records,
                old_relay_id=164,
                leader_id=170,
                current_relays=[182],
                stage="relay rejoin",
            )

    def test_old_relay_rejoin_rejects_direct_leader_parent_when_replacement_exists(self):
        topology = {
            "parent_by_member": {"164": 170, "182": 170, "37": 182},
            "route_next_by_member": {"164": 170, "182": 170, "37": 182},
            "tree_lines": [],
        }
        records = {
            164: {"online": 1, "relay": 0},
            182: {"online": 1, "relay": 1},
            37: {"online": 1, "relay": 0},
        }

        with self.assertRaisesRegex(RuntimeError, "rejoined direct to leader while replacement relay exists"):
            nr._assert_old_relay_rejoins_as_child_or_member(
                topology,
                records,
                old_relay_id=164,
                leader_id=170,
                current_relays=[182],
                stage="relay rejoin",
            )

    def test_old_relay_rejoin_accepts_child_under_replacement_relay(self):
        topology = {
            "parent_by_member": {"164": 182, "182": 170, "37": 182},
            "route_next_by_member": {"164": 182, "182": 170, "37": 182},
            "tree_lines": [],
        }
        records = {
            164: {"online": 1, "relay": 0},
            182: {"online": 1, "relay": 1},
            37: {"online": 1, "relay": 0},
        }

        nr._assert_old_relay_rejoins_as_child_or_member(
            topology,
            records,
            old_relay_id=164,
            leader_id=170,
            current_relays=[182],
            stage="relay rejoin",
        )

    def test_snapshot_peer_join_state_returns_last_state_without_failing(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader]
        member = lc.Peer(name="member", port="COMM", baudrate=115200, ser=mock.Mock(), log=[])

        with mock.patch.object(nr.fb, "_send_and_collect", return_value="team=25 self=164 joined=0 state=2") as send_collect:
            last = nr._snapshot_peer_join_state(
                member,
                peers,
                note="relay member",
            )

        self.assertIn("joined=0", last)
        send_collect.assert_called()

    def test_topology_uses_member_record_parent_and_next_when_route_notes_are_absent(self):
        records = {
            37: {"online": 1, "relay": 1, "tier": 1, "parent_id": 170, "next_hop": 170},
            164: {"online": 1, "relay": 0, "tier": 0, "parent_id": 37, "next_hop": 37},
            182: {"online": 1, "relay": 0, "tier": 0, "parent_id": 37, "next_hop": 37},
        }
        peer_by_id = {
            37: lc.Peer(name="member3", port="COM36", baudrate=115200, ser=mock.Mock(), log=[]),
            164: lc.Peer(name="member1", port="COM15", baudrate=115200, ser=mock.Mock(), log=[]),
            182: lc.Peer(name="member2", port="COM27", baudrate=115200, ser=mock.Mock(), log=[]),
        }

        topology = nr._topology_from_records(
            records,
            member_ids=[164, 182, 37],
            leader_id=170,
            leader_port="COM14",
            peer_by_id=peer_by_id,
            leader_log_text="",
        )

        self.assertEqual(topology["parent_by_member"]["164"], 37)
        self.assertEqual(topology["parent_by_member"]["182"], 37)
        self.assertEqual(topology["parent_by_member"]["37"], 170)

    def test_wait_topology_ready_falls_back_to_member_table_when_route_gate_does_not_refresh(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader]
        records = {
            37: {"online": 1, "relay": 1},
            164: {"online": 1, "relay": 0},
            182: {"online": 1, "relay": 0},
        }

        with mock.patch.object(nr.fb, "_wait_member_records", return_value=records) as wait_members, \
             mock.patch.object(
                 nr.fb,
                 "_wait_stable_final_topology",
                 side_effect=RuntimeError("timeout waiting for stable final topology: stable=0/4 direct_cap=1 route_ok=False last={}"),
             ) as wait_topology:
            result = nr._wait_topology_ready(
                leader,
                peers,
                leader_id=170,
                member_ids=[164, 182, 37],
                direct_cap=1,
                timeout_s=1.0,
                poll_s=0.1,
                log_start=0,
                stable_polls=2,
                note="initial enrollment",
            )

        self.assertIs(result, records)
        wait_members.assert_called_once()
        wait_topology.assert_called_once()

    def test_wait_natural_enrollment_uses_allow_all_without_pairing_window(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=mock.Mock(), log=["leader-log\n"])
        member = lc.Peer(name="member1", port="COM1", baudrate=115200, ser=mock.Mock(), log=[])
        peers = [leader, member]
        records = {
            164: {"online": 1, "relay": 1},
            182: {"online": 1, "relay": 0},
        }

        with mock.patch.object(nr, "_wait_topology_ready", return_value=records) as wait_topology, \
             mock.patch.object(nr, "_snapshot_peer_join_state", return_value="joined=0") as snapshot:
            out_records, relays, topology = nr._wait_natural_enrollment(
                leader,
                [member],
                peers,
                leader_id=170,
                member_ids=[164, 182],
                direct_cap=1,
                timeout_s=1.0,
                poll_s=0.1,
                log_start=0,
                stable_polls=2,
                leader_port="COML",
                peer_by_id={164: member, 182: member},
                note="initial enrollment",
            )

        self.assertIs(out_records, records)
        self.assertEqual(relays, [164])
        self.assertEqual(topology["parent_by_member"]["164"], 170)
        wait_topology.assert_called_once()
        snapshot.assert_called_once()


if __name__ == "__main__":
    unittest.main()
