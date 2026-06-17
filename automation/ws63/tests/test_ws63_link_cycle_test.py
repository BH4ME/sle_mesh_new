import unittest

from automation.ws63.tools import ws63_link_cycle_test as lc


class FakeSerial:
    def __init__(self, data_chunks=None):
        self.data_chunks = list(data_chunks or [])
        self.writes = []
        self.closed = False

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        return None

    def read(self, _n):
        if self.data_chunks:
            return self.data_chunks.pop(0)
        return b""

    def reset_input_buffer(self):
        return None

    def reset_output_buffer(self):
        return None

    def close(self):
        self.closed = True


class LinkCycleUnitTest(unittest.TestCase):
    def test_send_line_uses_bounded_drain_instead_of_flush(self):
        class FlushFailsSerial(FakeSerial):
            @property
            def out_waiting(self):
                return 0

            def flush(self):
                raise AssertionError("send_line should not use blocking flush")

        peer = lc.Peer(name="member", port="/dev/null", baudrate=115200, ser=FlushFailsSerial())

        peer.send_line("leave")

        self.assertEqual(peer.ser.writes, [b"leave\r\n"])
        self.assertEqual(peer.log, [])
        self.assertEqual(peer.tx_log, ["leave"])

    def test_write_and_drain_times_out_when_driver_queue_stays_busy(self):
        class StuckSerial(FakeSerial):
            @property
            def out_waiting(self):
                return 1

        with self.assertRaisesRegex(RuntimeError, "serial write drain timeout"):
            lc._write_and_drain(StuckSerial(), b"leave\r\n", note="stuck", timeout_s=0.01)

    def test_query_member_online_parses_latest_state(self):
        leader = lc.Peer(name="leader", port="/dev/null", baudrate=115200, ser=FakeSerial())
        member = lc.Peer(name="member", port="/dev/null", baudrate=115200, ser=FakeSerial())

        leader.ser.data_chunks = [
            b"member=2 role=2 online=1 battery=90\n",
            b"member=2 role=2 online=0 battery=89\n",
        ]
        state = lc._query_member_online(leader, [leader, member], member_id=2, query_window_s=0.1)
        self.assertEqual(state, 0)

    def test_wait_regex_matches_target_peer_only(self):
        leader = lc.Peer(name="leader", port="/dev/null", baudrate=115200, ser=FakeSerial([b"pairing start ret=0\n"]))
        member = lc.Peer(name="member", port="/dev/null", baudrate=115200, ser=FakeSerial([b"noise\n"]))
        lc._wait_regex(leader, [leader, member], r"pairing start ret=0", timeout_s=0.2, note="pairing")

    def test_wait_leader_offline_event_accepts_relay_offline_log(self):
        leader = lc.Peer(
            name="leader",
            port="/dev/null",
            baudrate=115200,
            ser=FakeSerial([b"[team] relay offline member=164\n"]),
        )

        lc._wait_leader_offline_event(
            leader,
            [leader],
            member_id=164,
            timeout_s=0.2,
            note="relay offline",
            log_start=0,
        )

    def test_default_channel_matches_firmware_default(self):
        parser = lc.build_parser()

        args = parser.parse_args(["--leader-port", "/dev/tty.fake0", "--member-port", "/dev/tty.fake1"])

        self.assertEqual(args.channel, 17)
        self.assertEqual(args.leader_id, -1)

    def test_route_id_from_suffix_handles_zero_and_broadcast(self):
        self.assertEqual(lc._route_id_from_suffix(0xC700), 74)
        self.assertEqual(lc._route_id_from_suffix(0x12FF), 52)
        self.assertEqual(lc._route_id_from_suffix(0x01A2), 194)

    def test_route_id_from_suffix_mixes_high_byte(self):
        self.assertEqual(lc._route_id_from_suffix(0x2277), 158)
        self.assertEqual(lc._route_id_from_suffix(0x2177), 127)
        self.assertNotEqual(lc._route_id_from_suffix(0x2277), lc._route_id_from_suffix(0x2177))

    def test_extract_suffix_from_label_and_mac(self):
        self.assertEqual(lc._extract_suffix("label=LC7E9"), 0xC7E9)
        self.assertEqual(
            lc._extract_suffix("mac=AA:BB:CC:DD:C7:E9"),
            0xC7E9,
        )
        self.assertIsNone(lc._extract_suffix("no suffix here"))

    def test_reboot_cycle_does_not_send_manual_rejoin_before_leave(self):
        leader = lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        member = lc.Peer(name="member", port="COMM", baudrate=115200, ser=FakeSerial())
        calls = []
        originals = {
            "_open_peer": lc._open_peer,
            "_drain": lc._drain,
            "_detect_suffix": lc._detect_suffix,
            "_wait_regex": lc._wait_regex,
            "_wait_member_state": lc._wait_member_state,
            "_wait_leader_offline_event": lc._wait_leader_offline_event,
            "_query_member_online": lc._query_member_online,
            "_wait_role": lc._wait_role,
        }

        def fake_open_peer(name, _port, _baudrate):
            return leader if name == "leader" else member

        def fake_drain(_peers, seconds):
            calls.append(("drain", seconds))

        def fake_detect_suffix(_peer, _peers, _provided_hex, _timeout_s, _note):
            calls.append(("detect_suffix",))
            return 0xC7E9

        def fake_wait_regex(target, _peers, pattern, _timeout_s, note):
            calls.append(("wait_regex", target.name, note, pattern))

        def fake_wait_member_state(_leader, _peers, _member_id, expect_online, _timeout_s, _poll_s):
            calls.append(("wait_member_state", expect_online))

        def fake_wait_leader_offline_event(_leader, _peers, *, member_id, timeout_s, note, log_start):
            calls.append(("offline_event", member_id, timeout_s, note, log_start))

        def fake_query_member_online(_leader, _peers, _member_id, _query_window_s=None, **_kwargs):
            calls.append(("query_member_online",))
            return 0

        def fake_wait_role(peer, _peers, expected_role, _timeout_s=None, _poll_s=None, **_kwargs):
            calls.append(("wait_role", peer.name, expected_role))

        try:
            lc._open_peer = fake_open_peer
            lc._drain = fake_drain
            lc._detect_suffix = fake_detect_suffix
            lc._wait_regex = fake_wait_regex
            lc._wait_member_state = fake_wait_member_state
            lc._wait_leader_offline_event = fake_wait_leader_offline_event
            lc._query_member_online = fake_query_member_online
            lc._wait_role = fake_wait_role

            args = lc.build_parser().parse_args([
                "--leader-port", "COML",
                "--member-port", "COMM",
                "--leader-id", "1",
                "--team-id", "1",
                "--member-id", "2",
                "--channel", "17",
                "--initial-drain-s", "0",
                "--no-auto-rejoin-s", "0.1",
            ])
            self.assertEqual(lc.run(args), 0)
        finally:
            for name, original in originals.items():
                setattr(lc, name, original)

        member_tx = list(member.tx_log)
        self.assertEqual(member_tx, ["join 1 1 17", "reboot", "leave", "role member C7E9"])
        reboot_index = member_tx.index("reboot")
        leave_index = member_tx.index("leave")
        self.assertFalse(any(cmd.startswith("join ") or cmd.startswith("role member ") for cmd in member_tx[reboot_index + 1:leave_index]))
        self.assertIn(
            ("wait_regex", "member", "member restore from NV after reboot", lc.MEMBER_RESTORE_PATTERN),
            calls,
        )
        reboot_slice = calls[: calls.index(("wait_regex", "member", "member leave", "leave ret=0"))]
        self.assertFalse(any(call[0] == "offline_event" for call in reboot_slice))
        self.assertTrue(any(call[:4] == ("offline_event", 2, 20.0, "leader offline after manual leave") for call in calls))


if __name__ == "__main__":
    unittest.main()
