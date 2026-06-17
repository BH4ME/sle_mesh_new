import unittest

from automation.ws63.tools import ws63_relay_cycle_test as rc


class FakeSerial:
    def __init__(self):
        self.writes = []
        self.closed = False

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        return None

    def read(self, _n):
        return b""

    def reset_input_buffer(self):
        return None

    def reset_output_buffer(self):
        return None

    def close(self):
        self.closed = True


class RelayCycleUnitTest(unittest.TestCase):
    def test_parse_member_records_reads_relay_fields(self):
        text = "\n".join([
            "member=13 role=0 online=1 battery=100 fix=1 pos_valid=1 rssi=-30 ready=1 relay=1 tier=1 max_down=8 last_seq=4 last_seen=10",
            "member=16 role=0 online=0 battery=99 fix=0 rssi=-80 relay=0 tier=0 max_down=0 last_seq=2 last_seen=12",
        ])

        records = rc._parse_member_records(text)

        self.assertEqual(records[13]["online"], 1)
        self.assertEqual(records[13]["relay"], 1)
        self.assertEqual(records[13]["max_down"], 8)
        self.assertEqual(records[16]["online"], 0)
        self.assertEqual(records[13]["rssi"], -30)
        self.assertEqual(records[13]["mac_ready"], 1)

    def test_parse_member_records_reads_mac_suffix_when_present(self):
        text = "member=13 role=0 online=1 battery=100 fix=1 rssi=-30 relay=1 tier=1 max_down=8 mac=2283 last_seq=4 last_seen=10"

        records = rc._parse_member_records(text)

        self.assertEqual(records[13]["mac_suffix"], 0x2283)

    def test_default_parser_requires_child_parent_relay(self):
        parser = rc.build_parser()

        args = parser.parse_args([
            "--leader-port",
            "COM1",
            "--relay-port",
            "COM2",
            "--child-port",
            "COM3",
        ])

        self.assertTrue(args.require_child_parent_relay)
        self.assertEqual(args.channel, 17)
        self.assertEqual(args.relay_reboot_command, "reboot")

    def test_relay_reboot_flow_does_not_send_leave_or_child_manual_rejoin(self):
        leader = rc.lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        relay = rc.lc.Peer(name="relay", port="COMR", baudrate=115200, ser=FakeSerial())
        child = rc.lc.Peer(name="child", port="COMC", baudrate=115200, ser=FakeSerial())
        calls = []
        originals = {
            "open_peer": rc.lc._open_peer,
            "drain": rc.lc._drain,
            "detect_suffix": rc.lc._detect_suffix,
            "wait_regex": rc.lc._wait_regex,
            "wait_role": rc.lc._wait_role,
            "wait_leader_offline_event": rc.lc._wait_leader_offline_event,
            "detect_route_id": rc._detect_route_id,
            "collect_leader_visibility": rc._collect_leader_visibility,
            "wait_member_record": rc._wait_member_record,
            "wait_peer_joined": rc._wait_peer_joined,
            "wait_log_pattern": rc._wait_log_pattern,
        }

        def fake_open_peer(name, _port, _baudrate):
            return {"leader": leader, "relay": relay, "child": child}[name]

        def fake_drain(_peers, seconds):
            calls.append(("drain", seconds))

        def fake_detect_suffix(_peer, _peers, _provided_hex, _timeout_s, _note):
            calls.append(("detect_suffix", _note))
            return 0x279A

        def fake_detect_route_id(peer, _peers, *, provided_id, timeout_s, note):
            calls.append(("detect_route_id", peer.name, provided_id, timeout_s, note))
            return 13 if peer is relay else 16

        def fake_wait_regex(target, _peers, pattern, _timeout_s, note):
            calls.append(("wait_regex", target.name, note, pattern))

        def fake_wait_role(peer, _peers, expected_role, _timeout_s=None, _poll_s=None, **_kwargs):
            calls.append(("wait_role", peer.name, expected_role))

        def fake_wait_leader_offline_event(_leader, _peers, *, member_id, timeout_s, note, log_start):
            calls.append(("offline_event", member_id, timeout_s, note, log_start))

        def fake_collect_leader_visibility(_leader, _peers, *, member_id, leader_id, timeout_s, poll_s):
            calls.append(("visibility", member_id, leader_id, timeout_s, poll_s))

        def fake_wait_member_record(_leader, _peers, *, member_id, expect_online, expect_relay, timeout_s, poll_s, note):
            calls.append(("member_record", member_id, expect_online, expect_relay, timeout_s, poll_s, note))
            return {"online": expect_online if expect_online is not None else 1, "relay": expect_relay or 0}

        def fake_wait_peer_joined(peer, _peers, *, timeout_s, poll_s, note):
            calls.append(("joined", peer.name, timeout_s, poll_s, note))

        def fake_wait_log_pattern(target, _peers, *, pattern, timeout_s, note, log_start=0):
            calls.append(("log_pattern", target.name, timeout_s, note, pattern, log_start))

        try:
            rc.lc._open_peer = fake_open_peer
            rc.lc._drain = fake_drain
            rc.lc._detect_suffix = fake_detect_suffix
            rc.lc._wait_regex = fake_wait_regex
            rc.lc._wait_role = fake_wait_role
            rc.lc._wait_leader_offline_event = fake_wait_leader_offline_event
            rc._detect_route_id = fake_detect_route_id
            rc._collect_leader_visibility = fake_collect_leader_visibility
            rc._wait_member_record = fake_wait_member_record
            rc._wait_peer_joined = fake_wait_peer_joined
            rc._wait_log_pattern = fake_wait_log_pattern

            args = rc.build_parser().parse_args([
                "--leader-port",
                "COML",
                "--relay-port",
                "COMR",
                "--child-port",
                "COMC",
                "--bootstrap-roles",
                "--skip-pos-report",
                "--initial-drain-s",
                "0",
            ])
            self.assertEqual(rc.run(args), 0)
        finally:
            rc.lc._open_peer = originals["open_peer"]
            rc.lc._drain = originals["drain"]
            rc.lc._detect_suffix = originals["detect_suffix"]
            rc.lc._wait_regex = originals["wait_regex"]
            rc.lc._wait_role = originals["wait_role"]
            rc.lc._wait_leader_offline_event = originals["wait_leader_offline_event"]
            rc._detect_route_id = originals["detect_route_id"]
            rc._collect_leader_visibility = originals["collect_leader_visibility"]
            rc._wait_member_record = originals["wait_member_record"]
            rc._wait_peer_joined = originals["wait_peer_joined"]
            rc._wait_log_pattern = originals["wait_log_pattern"]

        leader_tx = list(leader.tx_log)
        relay_tx = list(relay.tx_log)
        child_tx = list(child.tx_log)

        self.assertEqual(
            leader_tx,
            [
                "role leader",
                "pairing start",
                "pairing approve 13 relay",
                "pairing approve 16 norelay",
                "pairing stop",
            ],
        )
        self.assertEqual(relay_tx, ["role member 279A", "reboot"])
        self.assertEqual(child_tx, ["role member 279A"])
        relay_reboot_index = relay_tx.index("reboot")
        self.assertFalse(any(cmd == "leave" for cmd in relay_tx[relay_reboot_index:]))
        self.assertFalse(any(cmd.startswith("role member ") or cmd.startswith("join ") for cmd in child_tx[1:]))
        offline_events = [call for call in calls if call[:4] == ("offline_event", 13, 30.0, "leader offline after relay reboot")]
        self.assertEqual(len(offline_events), 1)
        self.assertIsInstance(offline_events[0][4], int)


if __name__ == "__main__":
    unittest.main()
