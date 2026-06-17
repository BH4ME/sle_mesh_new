import re
import unittest
from pathlib import Path
from unittest import mock

from automation.ws63.tools import ws63_four_board_relay_test as fb
from automation.ws63.tools import ws63_remote_build_v4 as rb
from automation.ws63.tools.ws63_route_id import route_id_from_suffix


EXPECTED_VERSION = "v4.5.46-minimal"
RETIRED_ACTIVE_TOKENS = [
    "TEAM_LEADER_NET_STAGE",
    "team_leader_topology_",
    "topology_frozen",
    "topology_policy",
    "stable_rejoin",
    "route_hint",
    "failover",
    "leader_logical_upstream",
    "try_parent_switch",
    "reselect",
    "DISCOVERING",
    "FREEZING",
    "PROVISIONING",
    "STABLE",
    "routeMetrics",
    "runtimeRelayBudget",
    "runtimeNetworkStage",
    "LeaderNetworkStage",
    "topology frozen",
    "topology policy",
]


class FakeSerial:
    def __init__(self):
        self.writes = []

    def write(self, data):
        self.writes.append(data)
        return len(data)

    def flush(self):
        return None


class FourBoardRelayUnitTest(unittest.TestCase):
    REPO_ROOT = Path(__file__).resolve().parents[3]

    @classmethod
    def _read(cls, rel_path: str) -> str:
        return (cls.REPO_ROOT / rel_path).read_text(encoding="utf-8")

    @staticmethod
    def _extract_define_int(source: str, name: str) -> int:
        match = re.search(rf"^#define\s+{re.escape(name)}\s+((?:0x)?[0-9A-Fa-f]+)U?\b", source, re.MULTILINE)
        if not match:
            raise AssertionError(f"define not found: {name}")
        return int(match.group(1), 0)

    def test_send_cli_line_uses_bounded_drain_instead_of_flush(self):
        class FlushFailsSerial(FakeSerial):
            @property
            def out_waiting(self):
                return 0

            def flush(self):
                raise AssertionError("_send_cli_line should not use blocking flush")

        peer = fb.lc.Peer(name="member", port="COMX", baudrate=115200, ser=FlushFailsSerial())

        fb._send_cli_line(peer, "leave")

        self.assertEqual(peer.ser.writes, [b"\r\n", b"leave\r\n"])
        self.assertEqual(peer.log, [])
        self.assertEqual(peer.tx_log, ["leave"])

    def test_remote_build_generated_config_script_compiles(self):
        compile(rb.build_config_script(), "generated_configure.py", "exec")

    def test_wait_leader_sees_member_accepts_already_online_record(self):
        leader = fb.lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        calls = []
        original_query = fb._query_records_once
        original_send = fb._send_and_collect

        def fake_query_records_once(_leader, _peers, window_s=1.0):
            calls.append(("members", window_s))
            return {241: {"online": 1, "relay": 0, "tier": 0, "max_down": 0, "last_seen": 10}}

        def fail_send_and_collect(*_args, **_kwargs):
            raise AssertionError("already-online member should not require pairing pending")

        try:
            fb._query_records_once = fake_query_records_once
            fb._send_and_collect = fail_send_and_collect

            fb._wait_leader_sees_member(
                leader,
                [leader],
                member_id=241,
                leader_id=154,
                timeout_s=1.0,
                poll_s=0.2,
            )
        finally:
            fb._query_records_once = original_query
            fb._send_and_collect = original_send

        self.assertEqual(calls, [("members", 0.2)])
        self.assertEqual(leader.ser.writes, [])

    def test_send_cli_once_and_wait_pattern_does_not_repeat_mutating_command(self):
        leader = fb.lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        calls = []
        original_send = fb._send_cli_line
        original_read = fb._read_available

        def fake_send(peer, line):
            calls.append(line)
            original_send(peer, line)
            if line == "cfg direct 1":
                peer.log.append("[cfg] direct cap=1 hw_max=8 ret=0\n")

        try:
            fb._send_cli_line = fake_send
            fb._read_available = lambda _peer: ""

            match = fb._send_cli_once_and_wait_pattern(
                leader,
                [leader],
                command="cfg direct 1",
                pattern=r"\[cfg\] direct cap=1\b.*ret=0",
                timeout_s=0.5,
                note="leader direct cap",
            )
        finally:
            fb._send_cli_line = original_send
            fb._read_available = original_read

        self.assertIsNotNone(match)
        self.assertEqual(calls.count("cfg direct 1"), 1)
        self.assertNotIn("cfg status", calls)

    def test_leader_runtime_fallback_accepts_configured_log_when_cfg_json_is_malformed(self):
        text = (
            "[team] configured fw=v4.5.46-minimal role=leader self=170 leader=170 "
            "team=24 channel=63 direct_cap=3 term=2 label=L2283\n"
            "[cfg] leader-now ret=0 team=24 channel=63 self_suffix=2283\n"
            '[cfg-json] {"ok":true,"fw":"v4.5.46-minimal","nvRolyTarget":0}\n'
        )

        status = fb._leader_runtime_from_config_log(
            text,
            team_id=24,
            channel=63,
            direct_cap=3,
            relay_target=None,
        )

        self.assertIsNotNone(status)
        self.assertEqual(status["runtimeRole"], "leader")
        self.assertEqual(status["runtimeSelf"], 170)
        self.assertEqual(status["runtimeDirectCap"], 3)
        self.assertEqual(status["runtimeLeaderTerm"], 2)

    def test_leader_runtime_fallback_defaults_missing_term_for_old_logs(self):
        text = (
            "[team] configured fw=v4.5.46-minimal role=leader self=170 leader=170 "
            "team=24 channel=63 direct_cap=3 label=L2283\n"
            "[cfg] leader-now ret=0 team=24 channel=63 self_suffix=2283\n"
        )

        status = fb._leader_runtime_from_config_log(
            text,
            team_id=24,
            channel=63,
            direct_cap=3,
            relay_target=None,
        )

        self.assertIsNotNone(status)
        self.assertEqual(status["runtimeLeaderTerm"], 1)

    def test_leader_runtime_fallback_requires_leader_now_success(self):
        text = (
            "[team] configured fw=v4.5.46-minimal role=leader self=170 leader=170 "
            "team=24 channel=63 direct_cap=3 label=L2283\n"
        )

        status = fb._leader_runtime_from_config_log(
            text,
            team_id=24,
            channel=63,
            direct_cap=3,
            relay_target=None,
        )

        self.assertIsNone(status)

    def test_configure_roles_preconfigures_direct_and_relay_target_before_leader_start(self):
        leader = fb.lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        events = []
        original_cfg_now = fb._send_cfg_and_wait
        original_wait_leader_runtime = fb._wait_leader_runtime_after_config
        original_send_once = fb._send_cli_once_and_wait_pattern
        original_member_role = fb._configure_member_role

        def fake_cfg_now(*_args, command, **_kwargs):
            if command.startswith("cfg direct") or command.startswith("cfg relay target"):
                raise AssertionError("direct/relay target must not use retrying _send_cfg_and_wait")
            events.append(command)
            return mock.Mock(group=lambda _index: "0")

        def fake_wait_leader_runtime(*_args, team_id, channel, direct_cap, relay_target, **_kwargs):
            self.assertEqual((team_id, channel, direct_cap, relay_target), (9, 41, 1, 1))
            events.append("wait runtime leader")
            return {"runtimeRole": "leader", "runtimeDirectCap": 1, "runtimeRelayTarget": 1}

        def fake_send_once(*_args, command, **_kwargs):
            events.append(command)
            return mock.Mock()

        try:
            fb._send_cfg_and_wait = fake_cfg_now
            fb._wait_leader_runtime_after_config = fake_wait_leader_runtime
            fb._send_cli_once_and_wait_pattern = fake_send_once
            fb._configure_member_role = lambda *_args, **_kwargs: None

            status = fb._configure_roles(
                leader,
                [],
                [leader],
                leader_suffix="C7E9",
                team_id=9,
                channel=41,
                direct_cap=1,
                relay_target=1,
                skip_direct_config=False,
                timeout_s=1.0,
                boot_timeout_s=1.0,
            )
        finally:
            fb._send_cfg_and_wait = original_cfg_now
            fb._wait_leader_runtime_after_config = original_wait_leader_runtime
            fb._send_cli_once_and_wait_pattern = original_send_once
            fb._configure_member_role = original_member_role

        self.assertEqual(
            events,
            [
                "cfg direct 1",
                "cfg relay target 1",
                "cfg leader now 9 41",
                "wait runtime leader",
            ],
        )
        self.assertEqual(status["runtimeDirectCap"], 1)
        self.assertEqual(status["runtimeRelayTarget"], 1)

    def test_relay_forward_observation_window_starts_before_role_configuration(self):
        script_source = self._read("automation/ws63/tools/ws63_four_board_relay_test.py")

        marker = "relay_forward_log_start = len(relay.log)"
        self.assertEqual(script_source.count(marker), 1)
        self.assertLess(script_source.index(marker), script_source.index("leader_status = _configure_roles("))

    def test_wait_relay_forwards_children_accepts_existing_child_traffic(self):
        relay = fb.lc.Peer(name="relay", port="COMR", baudrate=115200, ser=FakeSerial())
        relay.log.extend(
            [
                "[team] client conn bind conn=1 route=182\n",
                "[team-rx] conn=1 side=client ret=0\n",
                "[team] client conn bind conn=2 route=37\n",
                "[team-rx] conn=2 side=client ret=0\n",
            ]
        )

        fb._wait_relay_forwards_children(relay, [relay], child_ids=[182, 37], timeout_s=0.1, log_start=0)

    def test_try_wait_child_relay_allows_fast_original_relay_restore(self):
        leader = fb.lc.Peer(name="leader", port="COML", baudrate=115200, ser=FakeSerial())
        events = []
        original_wait = fb._wait_any_child_relay
        original_progress = fb._progress

        def fake_wait(*_args, **_kwargs):
            raise RuntimeError("no child relay elected after relay loss, last={164: {'online': 1, 'relay': 1}}")

        try:
            fb._wait_any_child_relay = fake_wait
            fb._progress = events.append

            records = fb._try_wait_child_relay(
                leader,
                [leader],
                child_ids=[182, 37],
                timeout_s=0.1,
                poll_s=0.1,
            )
        finally:
            fb._wait_any_child_relay = original_wait
            fb._progress = original_progress

        self.assertEqual(records, {})
        self.assertTrue(any("child relay election not observed" in event for event in events))

    def test_minimal_firmware_source_contract(self):
        app_source = self._read("xc/ws63_team_network/src/ws63_team_network_app.c")
        node_source = self._read("src/sle_team_node.c")
        optimizer_source = self._read("src/sle_team_relay_optimizer.c")
        location_source = self._read("src/sle_team_location.c")
        node_header = self._read("include/sle_team_node.h")
        cli_source = self._read("src/sle_team_cli.c")
        client_source = self._read("xc/ws63_team_network/sle_uart_client/sle_uart_client.c")
        server_source = self._read("xc/ws63_team_network/sle_uart_server/sle_uart_server.c")
        status_led_source = self._read("xc/ws63_team_network/src/ws63_team_status_led.c")
        ws2812_source = self._read("xc/ws63_team_network/src/ws63_ws2812.c")
        st7789_source = self._read("xc/ws63_team_network/src/ws63_st7789_display.c")
        http_source = self._read("xc/ws63_team_network/src/ws63_team_http.c")
        cmake_source = self._read("xc/ws63_team_network/CMakeLists.txt")

        self.assertIn(f'#define SLE_TEAM_FW_VERSION "{EXPECTED_VERSION}"', app_source)
        self.assertIn("#define SLE_TEAM_FW_COMPAT 0x0546U", app_source)
        self.assertIn("cfg.fw_compat = (uint16_t)SLE_TEAM_FW_COMPAT;", app_source)
        self.assertIn("team_fw_compat_from_adv_data", app_source)
        self.assertIn("fw_compat != SLE_TEAM_FW_COMPAT", app_source)
        self.assertIn("drop rejected hello", app_source)
        self.assertIn("sle_uart_client_disconnect_conn(conn_id)", app_source)
        self.assertIn("fw_compat_lo", node_source)
        self.assertIn("fw_compat_hi", node_source)
        self.assertIn("hello_fw_compat != node->cfg.fw_compat", node_source)
        self.assertNotIn("hello_fw_compat != SLE_TEAM_FW_COMPAT_ANY", node_source)
        self.assertIn('#define SLE_TEAM_HW_CONSTRAINTS "minimal leader/member/relay rewrite"', app_source)
        self.assertIn("#define SLE_TEAM_NV_CONFIG_VERSION 4U", app_source)
        self.assertIn("leader_term", node_header)
        self.assertIn("runtimeLeaderTerm", app_source)
        self.assertEqual(self._extract_define_int(app_source, "SLE_TEAM_DIRECT_CAP_DEFAULT"), 7)
        self.assertEqual(self._extract_define_int(node_source, "SLE_TEAM_DIRECT_CAP_DEFAULT"), 7)
        self.assertEqual(self._extract_define_int(node_source, "SLE_TEAM_RELAY_CHILD_CAP_DEFAULT"), 7)
        self.assertLessEqual(len(app_source.splitlines()), 2200)
        self.assertLessEqual(len(node_source.splitlines()), 2200)
        self.assertIn('#include "ws63_st7789_display.h"', app_source)
        self.assertIn("#define SLE_TEAM_DISPLAY_TASK_STACK_SIZE 0x1800", app_source)
        self.assertIn("#define SLE_TEAM_DISPLAY_TASK_PRIO 30", app_source)
        self.assertGreaterEqual(
            self._extract_define_int(app_source, "SLE_TEAM_DISPLAY_TASK_STACK_SIZE"),
            self._extract_define_int(app_source, "SLE_TEAM_APP_TASK_STACK_SIZE"),
        )
        self.assertGreater(
            self._extract_define_int(app_source, "SLE_TEAM_DISPLAY_TASK_PRIO"),
            self._extract_define_int(app_source, "SLE_TEAM_APP_TASK_PRIO"),
        )
        self.assertIn('"TeamDisplayTask"', app_source)
        self.assertIn("team_display_spawn_task();", app_source)
        self.assertIn("ws63_st7789_init(&cfg)", app_source)
        self.assertIn("ws63_st7789_show_status", app_source)
        self.assertIn("ws63_st7789_show_event", app_source)
        self.assertIn("ws63_st7789_tick();", app_source)
        self.assertIn("team_display_publish_status()", app_source)
        self.assertIn("team_display_publish_event(", app_source)
        network_task_body = app_source.split('static void *team_network_task(const char *arg)', 1)[1].split(
            'static int team_display_init_panel(void)', 1
        )[0]
        self.assertNotIn("ws63_st7789_tick();", network_task_body)
        self.assertIn("ws63_st7789_show_status", st7789_source)
        self.assertIn("ws63_st7789_show_event", st7789_source)
        self.assertIn("ws63_st7789_tick", st7789_source)
        self.assertIn("sle_team_relay_optimizer_run", optimizer_source)
        self.assertNotIn("sle_team_leader_migration", app_source)
        self.assertNotIn("leader migrate plan", app_source)
        self.assertIn("incoming_term > node->cfg.leader_term", node_source)
        self.assertIn("SLE_TEAM_OPT_RSSI_HYSTERESIS_DB", optimizer_source)
        self.assertIn("member->child_count != 0U", optimizer_source)
        self.assertIn("node->relay_recovery_pending != 0U", optimizer_source)
        self.assertIn("member->policy_pending != 0U", optimizer_source)
        self.assertIn("member->relay_recovery_candidate != 0U", optimizer_source)
        self.assertIn("sle_team_relay_optimizer_tick(&g_team_node", app_source)
        self.assertIn("8000U", app_source)
        self.assertIn(
            "test_returning_old_relay_after_rssi_swap_joins_current_relay",
            self._read("examples/team_node_regression_test.c"),
        )
        self.assertIn("positionValid", self._read("src/sle_team_web_api.c"))
        self.assertIn("sle_team_node_record_local_position", node_header)
        self.assertIn("sle_team_node_record_local_position", location_source)
        self.assertNotIn("member->online = 1U", location_source)
        self.assertIn("sle_team_node_record_local_position", http_source)
        self.assertIn("static sle_team_web_event_log_t g_team_events;", app_source)
        self.assertIn("sle_team_web_event_log_init(&g_team_events)", app_source)
        self.assertIn("ws63_team_http_start(&g_team_node, &g_team_events", app_source)
        self.assertNotIn("ws63_team_http_start(&g_team_node, NULL", app_source)
        self.assertIn("src/ws63_st7789_display.c", cmake_source)
        self.assertIn("src/sle_team_location.c", cmake_source)
        self.assertNotIn("src/sle_team_leader_migration.c", cmake_source)
        self.assertIn("lat=%ld lon=%ld speed=%u heading=%u sat=%u", cli_source)
        web_client = self._read("webui/src/api/client.ts")
        self.assertIn(r"\blat=(-?\d+).*?\blon=(-?\d+).*?\bspeed=(\d+).*?\bheading=(\d+).*?\bsat=(\d+)", web_client)
        self.assertIn("positionValid: match[7] ? Number(match[7]) !== 0 : Number(match[6]) !== 0", web_client)
        self.assertIn("latitudeE6: Number(match[8])", web_client)
        self.assertIn("longitudeE6: Number(match[9])", web_client)

        for source_name, source in {
            "app": app_source,
            "node": node_source,
        }.items():
            for token in RETIRED_ACTIVE_TOKENS:
                self.assertNotIn(token, source, f"{source_name} still contains retired token {token}")

        for item in [
            "sle_team_assign_parent",
            "sle_team_select_online_relay",
            "sle_team_node_grant_relay",
            "sle_team_node_member_link_lost",
            "sle_team_forward_packet",
            "SLE_TEAM_NET_WAIT_POLICY",
            "SLE_TEAM_PARENT_WAIT_POLICY",
        ]:
            self.assertIn(item, node_source)

        for item in [
            "policy_pending;",
            "parent_id;",
            "next_hop_id;",
            "child_count;",
            "relay_recovery_candidate;",
            "relay_recovery_pending;",
            "relay_recovery_lost_relay_id;",
            "relay_recovery_selected_id;",
        ]:
            self.assertIn(item, node_header)
        self.assertIn("member->policy_pending = 1U", node_source)
        self.assertIn("keep_existing_policy", node_source)
        self.assertIn("sle_team_resend_member_policy", node_source)
        self.assertIn("member->online != 0U &&", node_source)
        self.assertIn("sle_team_choose_relay_recovery_candidate", node_source)
        self.assertIn("replacement relay online", node_source)
        self.assertIn("relay recovery pending", node_source)
        self.assertIn("sle_team_confirm_member_online", node_source)
        self.assertNotIn("parent_id = node->cfg.leader_id;\n        }\n    }\n    member->parent_id = parent_id", node_source)
        self.assertIn("SLE_TEAM_NET_WAIT_POLICY = 1", node_header)
        self.assertIn("SLE_TEAM_PARENT_WAIT_POLICY = 1", node_header)
        self.assertNotIn("sle_team_node_try_parent_switch", node_header)

        for item in [
            "[cfg-json]",
            "cfg leader now",
            "cfg member now",
            "cfg direct",
            "cfg relay target",
            "cfg.direct_cap = direct_cap",
            "g_team_rt.direct_cap = cfg->direct_cap",
            "team_leader_needs_auto_overflow_relay",
            "team_leader_relay_target_pending",
            "team_leader_relay_recovery_pending",
            "relay_recovery_selected_id",
            "relay-offline-recovery",
            "relay-recovery-ready",
            "relay_target_pending != 0U",
            "team_physical_connect_limit",
            "team_member_disable_relay_client",
            "team_relay_client_start_if_ready",
            "sle_uart_client_init(team_client_rx_cb, team_client_rx_cb)",
            "sle_uart_server_init(team_server_read_cb, team_server_write_cb)",
            "team_leader_direct_occupied_count",
            "sle_uart_client_send_by_conn",
            "sle_uart_server_send_report_by_handle",
            "sle_uart_server_send_report_by_conn(server_conn_id, buf, len)",
            "sle_uart_server_find_conn_by_member_ex(g_team_node.upstream_parent_id, &server_conn_id)",
            "sle_uart_client_find_conn_by_member(member->member_id, &conn_id)",
        ]:
            self.assertIn(item, app_source)
        self.assertIn('team_member_disable_relay_client("relay-not-allowed");', app_source)
        self.assertIn("if (team_leader_needs_auto_overflow_relay() != 0U) {", app_source)
        self.assertIn('sle_uart_client_resume_scan("leader-direct-lost")', app_source)
        self.assertIn("sle_team_node_member_offline(&g_team_node, disconnected_member_id)", app_source)
        self.assertIn("team_leader_resolve_disconnected_member", app_source)
        self.assertIn("team_leader_known_relay_child", app_source)
        self.assertIn("disconnect keep relay child", app_source)
        self.assertIn("disconnect ignored conn=%u route=%u", app_source)
        self.assertIn("stale upstream disconnect", app_source)
        self.assertIn("sle_uart_server_get_conn_member(conn_id, &disconnected_member_id)", app_source)
        self.assertIn("disconnected_member_id == g_team_node.upstream_parent_id", app_source)
        self.assertIn("uint8_t upstream_lost", app_source)
        self.assertIn("disconnect ignored conn=%u no bound upstream while joined parent=%u", app_source)
        self.assertNotIn(
            "disconnected_member_id == g_team_node.upstream_parent_id || sle_uart_server_connected_count() == 0U",
            app_source,
        )
        self.assertIn("team_route_id_from_mac(addr->addr)", app_source)
        self.assertIn('osal_printk("[team] disconnect resolved conn=%u member=%u', app_source)
        self.assertIn("int sle_team_node_member_offline(sle_team_node_t *node, uint8_t member_id);", node_header)

        for item in ["members", "state", "pairing start", "pairing stop"]:
            self.assertIn(item, cli_source)
        self.assertIn("sle_uart_client_send_by_conn", client_source)
        self.assertIn("sle_uart_server_send_report_by_conn", server_source)
        self.assertIn("team_leader_drop_stale_direct_conn", app_source)
        self.assertIn("drop stale upstream conn", app_source)
        self.assertIn("drop stale direct conn", app_source)
        self.assertIn("keep pending direct conn", app_source)
        self.assertIn("drop pending stale direct conn", app_source)
        self.assertIn("team_leader_drop_stale_direct_conn(conn_id, from_client, &app_packet) != 0U", app_source)
        self.assertIn("member->policy_pending != 0U &&", app_source)
        self.assertIn("member->next_hop_id != 0U &&", app_source)
        self.assertNotIn(
            "member->policy_pending != 0U && sle_uart_client_find_conn_by_member(member->member_id, conn_id)",
            app_source,
        )
        self.assertIn("physical_id = member->next_hop_id;", app_source)
        self.assertLess(
            app_source.index("member->next_hop_id != 0U &&"),
            app_source.index("physical_id = member->next_hop_id;"),
        )
        self.assertIn("test_pending_relay_child_position_confirms_through_current_relay", self._read("examples/team_node_regression_test.c"))
        self.assertIn("test_leader_rejects_mismatched_firmware_hello", self._read("examples/team_node_regression_test.c"))
        self.assertIn("test_leader_rejects_missing_firmware_hello", self._read("examples/team_node_regression_test.c"))
        self.assertIn("test_repeated_hello_reassigns_pending_member_to_direct_but_keeps_relay_delivery_hop", self._read("examples/team_node_regression_test.c"))
        self.assertNotIn("SLE_UART_WAIT_SLE_CORE_READY_MS", client_source)
        self.assertNotIn("osal_msleep(5000)", client_source)
        self.assertIn("SLE_UART_CONNECT_INFLIGHT_TIMEOUT_MS", client_source)
        self.assertIn("SLE_UART_FORCE_RESCAN_MIN_INTERVAL_MS", client_source)
        self.assertIn("SLE_UART_SEEK_SAMPLE_LOG_INTERVAL_MS", client_source)
        self.assertGreaterEqual(self._extract_define_int(app_source, "SLE_TEAM_LEADER_RESCAN_INTERVAL_MS"), 3000)
        self.assertGreaterEqual(self._extract_define_int(client_source, "SLE_UART_FORCE_RESCAN_MIN_INTERVAL_MS"), 2500)
        self.assertGreaterEqual(self._extract_define_int(client_source, "SLE_UART_SEEK_SAMPLE_LOG_INTERVAL_MS"), 8000)
        self.assertIn("sle_uart_client_should_sample_log", client_source)
        self.assertIn("should_connect = (uint8_t)", client_source)
        self.assertNotIn("seek data head", client_source)
        self.assertIn("g_sle_uart_connect_start_ms", client_source)
        self.assertIn("ignore disconnect conn_id:%u active:", client_source)
        self.assertIn("ignore state-none conn_id:%u active:", client_source)
        self.assertIn("sle_uart_client_addr_equal(&conn->addr, disconnect_addr_ptr) == 0U", client_source)
        self.assertIn("sle_uart_client_mark_conn_ready(conn_id, \"exchange-info\")", client_source)
        self.assertIn("sle_uart_client_mark_conn_ready(conn_id, \"property-discovery\")", client_source)
        self.assertIn("conn == NULL || conn->ready == 0U", client_source)
        self.assertIn("sle_uart_client_exchange_once(conn_id, \"send-not-ready\")", client_source)
        self.assertNotIn("leader skip route=%u relay_recovery_pending=1 selected", app_source)
        self.assertNotIn("leader skip route=%u relay_recovery_pending=1 candidate=0", app_source)
        self.assertNotIn("leader skip route=%u known_relay_child=1", app_source)
        self.assertNotIn("leader skip route=%u relay_target_pending=1", app_source)
        self.assertNotIn("leader skip route=%u direct_full=1", app_source)
        self.assertIn("#define TEAM_LED_PHASE_COUNT 8U", status_led_source)
        self.assertIn("#define TEAM_LED_TICK_MS 120U", status_led_source)
        self.assertEqual(self._extract_define_int(status_led_source, "TEAM_LED_IDLE_MAX_SCALE"), 8)
        self.assertEqual(self._extract_define_int(status_led_source, "TEAM_LED_NORMAL_MAX_SCALE"), 12)
        self.assertEqual(self._extract_define_int(status_led_source, "TEAM_LED_ALERT_MAX_SCALE"), 20)
        self.assertIn("g_status_led_breathe_idle", status_led_source)
        self.assertIn("g_status_led_breathe_normal", status_led_source)
        self.assertIn("g_status_led_breathe_alert", status_led_source)
        self.assertIn("{0U, 2U, 4U, 6U, 8U, 6U, 4U, 2U}", status_led_source)
        self.assertIn("{2U, 4U, 6U, 10U, 12U, 10U, 6U, 4U}", status_led_source)
        self.assertIn("{4U, 8U, 12U, 16U, 20U, 16U, 12U, 8U}", status_led_source)
        self.assertIn("status_led_apply_scaled", status_led_source)
        self.assertIn("status_led_scale_u8", status_led_source)
        self.assertNotIn("blink_phase", status_led_source)
        self.assertNotIn("status_led_apply(255U", status_led_source)
        self.assertNotIn("status_led_apply(160U", status_led_source)
        self.assertIn("if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) { ws63_team_status_led_leader(); return; }", app_source)
        self.assertIn("if (g_team_node.joined == 0U) { ws63_team_status_led_joining(); return; }", app_source)
        self.assertIn("if (g_team_node.cfg.relay_enabled != 0U || g_team_node.cfg.relay_allowed != 0U) { ws63_team_status_led_relay(); return; }", app_source)
        self.assertIn("if (g_team_node.upstream_parent_id != 0U && g_team_node.upstream_parent_id != g_team_node.cfg.leader_id) { ws63_team_status_led_child(); return; }", app_source)
        self.assertIn('volatile("rdcycle %0"', ws2812_source)
        self.assertIn("ws63_ws2812_calibrate_timing", ws2812_source)
        self.assertIn("WS63_WS2812_BOOT_TEST_MS", ws2812_source)
        self.assertIn("osal_msleep(WS63_WS2812_BOOT_TEST_MS)", ws2812_source)
        self.assertNotIn("WS63_WS2812_CPU_MHZ", ws2812_source)

    def test_build_and_flash_contracts_use_minimal_version(self):
        remote_build = self._read("automation/ws63/tools/ws63_remote_build_v4.py")
        local_wsl = self._read("scripts/build/ws63_build_v4_local_wsl.sh")
        ubuntu = self._read("scripts/build/ws63_build_v4_ubuntu.sh")
        multi_flash = self._read("scripts/flash/ws63_flash_multi.ps1")
        auto_burn = self._read("automation/ws63/tools/ws63_auto_burn.py")
        audit_versions = self._read("automation/ws63/tools/ws63_audit_versions_and_idle_bad.py")

        self.assertIn(f'VERSION = "{EXPECTED_VERSION}"', remote_build)
        self.assertIn("post-build guard passed", remote_build)
        self.assertIn("minimal networking", remote_build)
        self.assertIn('set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_ENABLE", "y")', remote_build)
        self.assertIn('"CONFIG_SLE_TEAM_GPS_ENABLE=y"', remote_build)
        self.assertIn('"CONFIG_SLE_TEAM_GPS_UART_BUS=1"', remote_build)
        self.assertIn('"CONFIG_SLE_TEAM_WS2812_ENABLE=y"', remote_build)
        self.assertIn('"ws63_team_gps.c.obj"', remote_build)
        self.assertIn('"ws63_team_status_led.c.obj"', remote_build)
        self.assertIn('"ws63_ws2812.c.obj"', remote_build)
        self.assertIn('"sle_team_nmea.c.obj"', remote_build)
        self.assertIn('"sle_team_nmea_parse_line"', remote_build)
        self.assertIn('"sle_team_nmea_feed"', remote_build)
        self.assertIn('"ws63_ws2812_set_rgb"', remote_build)
        self.assertIn('"ws63_team_http_start(&g_team_node, &g_team_events"', remote_build)
        self.assertNotIn('unset_kconfig_bool(s, "CONFIG_SLE_TEAM_GPS_ENABLE")', remote_build)
        for script in (local_wsl, ubuntu):
            self.assertIn("next_archive_path()", script)
            self.assertIn(f'ARCHIVE_OUT="$(next_archive_path "$LOCAL_OUT" "{EXPECTED_VERSION}")"', script)
            self.assertIn("post-build guard passed", script)
            self.assertIn('cp "$ARCHIVE_OUT" "$LOCAL_OUT"', script)
            self.assertIn('set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_ENABLE", "y")', script)
            self.assertIn('"CONFIG_SLE_TEAM_GPS_ENABLE=y"', script)
            self.assertIn('"CONFIG_SLE_TEAM_GPS_UART_BUS=1"', script)
            self.assertIn('"CONFIG_SLE_TEAM_WS2812_ENABLE=y"', script)
            self.assertIn('"ws63_team_gps.c.obj"', script)
            self.assertIn('"ws63_team_status_led.c.obj"', script)
            self.assertIn('"ws63_ws2812.c.obj"', script)
            self.assertIn('"sle_team_nmea.c.obj"', script)
            self.assertIn('"sle_team_nmea_parse_line"', script)
            self.assertIn('"sle_team_nmea_feed"', script)
            self.assertIn('"ws63_ws2812_set_rgb"', script)
            self.assertIn('"ws63_team_http_start(&g_team_node, &g_team_events"', script)
            self.assertNotIn('unset_kconfig_bool(s, "CONFIG_SLE_TEAM_GPS_ENABLE")', script)
        self.assertIn(f'[string]$ExpectedVersion = "{EXPECTED_VERSION}"', multi_flash)
        self.assertIn(f'DEFAULT_EXPECTED_FW_VERSION = "{EXPECTED_VERSION}"', auto_burn)
        self.assertIn(f'DEFAULT_EXPECTED_FW = "{EXPECTED_VERSION}"', audit_versions)

    def test_leader_scan_uses_occupied_direct_count_for_capacity(self):
        app_source = self._read("xc/ws63_team_network/src/ws63_team_network_app.c")

        self.assertIn(
            "if (team_leader_direct_occupied_count() < team_direct_cap()) {",
            app_source,
        )
        self.assertNotIn(
            "if (team_leader_direct_online_count() < team_direct_cap()) {",
            app_source,
        )
        self.assertIn("team_leader_needs_auto_overflow_relay()", app_source)
        self.assertIn("member->relay_allowed == 0U", app_source)
        self.assertNotIn("team_relay_count() != 0U", app_source)
        self.assertIn(
            "if (direct_full != 0U && relay_allowed == 0U && auto_overflow_relay_needed == 0U) {",
            app_source,
        )
        self.assertIn("test_overflow_promotes_second_relay_when_existing_relay_is_full", self._read("examples/team_node_regression_test.c"))

    def test_rewrite_task_book_rejects_old_patch_chain(self):
        task_book = self._read("docs/v4/ws63_network_rewrite_task_book.md")

        self.assertIn("Stop patching", task_book)
        self.assertIn("smallest usable leader/member/relay logic", task_book)
        self.assertIn("Leader stable direct capacity is `direct_cap=7`", task_book)
        self.assertIn("Relay forwards packets", task_book)
        self.assertIn("Do not claim \"networking is normal\" until hardware logs prove", task_book)

    def test_route_id_from_suffix_mixes_high_byte(self):
        self.assertEqual(route_id_from_suffix(0xC7E9), 53)
        self.assertEqual(route_id_from_suffix(0xC700), 74)
        self.assertEqual(route_id_from_suffix(0x12FF), 52)
        self.assertEqual(route_id_from_suffix(0x2277), 158)
        self.assertEqual(route_id_from_suffix(0x2177), 127)
        self.assertNotEqual(route_id_from_suffix(0x2277), route_id_from_suffix(0x2177))


if __name__ == "__main__":
    unittest.main()
