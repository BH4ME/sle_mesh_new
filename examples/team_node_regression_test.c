#include "sle_team_node.h"
#include "sle_team_packet.h"
#include "sle_team_nmea.h"
#include "sle_team_relay_optimizer.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t now_s;
    uint8_t last_tx[256];
    uint16_t last_tx_len;
    uint8_t fail_next_tx;
    uint8_t joined_count;
    uint8_t relay_offline_count;
    const char *name;
} test_runtime_t;

static int test_send(void *user_ctx, sle_team_send_kind_t kind, uint8_t dst_id, const uint8_t *buf, uint16_t len)
{
    test_runtime_t *rt = (test_runtime_t *)user_ctx;

    (void)kind;
    (void)dst_id;
    if (rt == NULL || buf == NULL || len > sizeof(rt->last_tx)) {
        return -1;
    }
    if (rt->fail_next_tx != 0U) {
        rt->fail_next_tx = 0U;
        return -1;
    }
    (void)memcpy(rt->last_tx, buf, len);
    rt->last_tx_len = len;
    return SLE_TEAM_OK;
}

static uint32_t test_now(void *user_ctx)
{
    const test_runtime_t *rt = (const test_runtime_t *)user_ctx;
    return rt == NULL ? 0U : rt->now_s;
}

static void test_joined(void *user_ctx, uint8_t member_id)
{
    test_runtime_t *rt = (test_runtime_t *)user_ctx;

    (void)member_id;
    if (rt != NULL) {
        rt->joined_count++;
    }
}

static void test_relay_offline(void *user_ctx, uint8_t member_id)
{
    test_runtime_t *rt = (test_runtime_t *)user_ctx;

    (void)member_id;
    if (rt != NULL) {
        rt->relay_offline_count++;
    }
}

static void test_init_node(sle_team_node_t *node, test_runtime_t *rt, uint8_t self_id,
    sle_team_node_role_t role)
{
    sle_team_node_cfg_t cfg;
    sle_team_node_ops_t ops;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.team_id = 1U;
    cfg.self_id = self_id;
    cfg.leader_id = role == SLE_TEAM_ROLE_LEADER ? self_id : 1U;
    cfg.role = role;
    cfg.channel_hash = 0x11U;
    cfg.heartbeat_interval_s = 1U;
    cfg.heartbeat_timeout_s = 3U;
    cfg.report_interval_s = 5U;
    cfg.default_ttl = 4U;
    cfg.fw_compat = 0x0541U;

    (void)memset(&ops, 0, sizeof(ops));
    ops.send = test_send;
    ops.now_s = test_now;
    ops.on_joined = test_joined;
    ops.on_relay_offline = test_relay_offline;
    ops.user_ctx = rt;

    assert(sle_team_node_init(node, &cfg, &ops) == SLE_TEAM_OK);
}

static void test_deliver_last(test_runtime_t *from, sle_team_node_t *to)
{
    if (from->last_tx_len == 0U) {
        return;
    }
    assert(sle_team_node_on_packet(to, from->last_tx, from->last_tx_len) == SLE_TEAM_OK);
    from->last_tx_len = 0U;
}

static uint8_t test_decode_last_app_packet(const test_runtime_t *rt, sle_team_app_packet_t *app)
{
    sle_team_mesh_packet_t mesh;
    const uint8_t *app_payload = NULL;
    uint16_t app_payload_len = 0U;
    uint8_t channel_hash = 0U;
    uint8_t cipher_mac[2];

    if (rt == NULL || app == NULL || rt->last_tx_len == 0U) {
        return 0U;
    }
    assert(sle_team_decode_mesh_packet(&mesh, rt->last_tx, rt->last_tx_len) == SLE_TEAM_OK);
    assert(sle_team_unwrap_mesh_group_data(&mesh, &channel_hash, cipher_mac, &app_payload, &app_payload_len) ==
        SLE_TEAM_OK);
    assert(channel_hash == 0x11U);
    return sle_team_decode_app_packet(app, app_payload, app_payload_len) == SLE_TEAM_OK ? 1U : 0U;
}

static void test_send_member_ack(sle_team_node_t *member, test_runtime_t *member_rt,
    uint16_t ack_seq, uint8_t acked_msg_type)
{
    assert(sle_team_node_send_ack(member, member->cfg.leader_id, ack_seq, acked_msg_type, 0U) == SLE_TEAM_OK);
    assert(member_rt->last_tx_len != 0U);
}

static void test_join_member(sle_team_node_t *leader, test_runtime_t *leader_rt,
    sle_team_node_t *member, test_runtime_t *member_rt)
{
    assert(sle_team_node_send_hello(member, member->cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(member_rt, leader);
    test_deliver_last(leader_rt, member);
    assert(member->joined != 0U);
    assert(member->state == SLE_TEAM_NET_ONLINE);
}

static void test_confirm_member_by_heartbeat(sle_team_node_t *leader, uint8_t member_id, uint32_t now_s)
{
    test_runtime_t member_rt = {.name = "confirm", .now_s = now_s};
    sle_team_node_t member;

    test_init_node(&member, &member_rt, member_id, SLE_TEAM_ROLE_MEMBER);
    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -40, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(leader, member_rt.last_tx, member_rt.last_tx_len) == SLE_TEAM_OK);
}

static void test_direct_join_and_link_lost_hello_loop(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t member_rt = {.name = "member", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t member;
    sle_team_app_packet_t app;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_join_member(&leader, &leader_rt, &member, &member_rt);

    assert(member.upstream_parent_id == 1U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_CONNECTED);
    assert(sle_team_node_find_member(&leader, 2U) != NULL);

    assert(sle_team_node_member_link_lost(&member) == SLE_TEAM_OK);
    assert(member.joined == 0U);
    assert(member.state == SLE_TEAM_NET_WAIT_POLICY);
    assert(member.upstream_parent_id == 0U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_WAIT_POLICY);

    member_rt.now_s = 2U;
    sle_team_node_tick(&member);
    assert(test_decode_last_app_packet(&member_rt, &app) != 0U);
    assert(app.app_msg_type == SLE_TEAM_APP_HELLO);
    assert(app.dst_id == member.cfg.leader_id);
}

static void test_member_leave_sends_leave_alert_before_idle(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 10U};
    test_runtime_t member_rt = {.name = "member", .now_s = 10U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_join_member(&leader, &leader_rt, &member, &member_rt);
    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -40, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);

    assert(sle_team_node_member_leave(&member) == SLE_TEAM_OK);
    assert(member.joined == 0U);
    assert(member.state == SLE_TEAM_NET_IDLE);
    assert(member.cfg.leader_id == 0U);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->parent_id == 0U);
    assert(record->next_hop_id == 0U);
}

static void test_leader_rejects_mismatched_firmware_hello(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t member_rt = {.name = "member", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t member;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.cfg.fw_compat = 0x0540U;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&leader, member_rt.last_tx, member_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    assert(sle_team_node_find_member(&leader, 2U) == NULL);
}

static void test_leader_rejects_missing_firmware_hello(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t member_rt = {.name = "member", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t member;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.cfg.fw_compat = SLE_TEAM_FW_COMPAT_ANY;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&leader, member_rt.last_tx, member_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    assert(sle_team_node_find_member(&leader, 2U) == NULL);
}

static void test_scan_response_fw_compat_and_route_id_from_raw_bytes(void)
{
    static const uint8_t sample[] = {
        0x0cU, 0x02U, 0x12U,
        0x06U, 0xffU, 0x53U, 0x4cU, 0xafU, 0x41U, 0x05U,
        0x10U, 0x0bU,
        0x73U, 0x6cU, 0x65U, 0x5fU, 0x75U, 0x61U, 0x72U, 0x74U, 0x5fU, 0x73U, 0x65U, 0x72U, 0x76U, 0x65U, 0x72U,
    };

    assert(sle_team_scan_route_id_from_data(sample, (uint16_t)sizeof(sample)) == 175U);
    assert(sle_team_scan_fw_compat_from_data(sample, (uint16_t)sizeof(sample)) == 0x0541U);
}

static void test_app_packet_carries_leader_term(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_app_packet_t app;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.cfg.leader_term = 7U;
    assert(sle_team_node_send_heartbeat(&leader, 2U, 100U, SLE_TEAM_RSSI_UNKNOWN, 1U) == SLE_TEAM_OK);
    assert(test_decode_last_app_packet(&leader_rt, &app) != 0U);
    assert(app.app_msg_type == SLE_TEAM_APP_HEARTBEAT);
    assert(app.leader_term == 7U);
}

static void test_member_select_new_leader_clears_stale_parent_and_relay_state(void)
{
    test_runtime_t member_rt = {.name = "member", .now_s = 20U};
    sle_team_node_t member;
    sle_team_app_packet_t app;

    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.joined = 1U;
    member.state = SLE_TEAM_NET_ONLINE;
    member.last_hello_s = 11U;
    member.last_heartbeat_s = 12U;
    member.last_config_s = 13U;
    member.last_leader_seen_s = 14U;
    member.last_parent_seen_s = 15U;
    member.upstream_parent_id = 3U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    member.cfg.relay_allowed = 1U;
    member.cfg.relay_enabled = 1U;
    member.cfg.relay_tier = 1U;
    member.cfg.max_downstream = 7U;
    member.cfg.relay_discovery_only = 1U;
    member_rt.last_tx_len = 0U;

    assert(sle_team_node_member_select_leader(&member, 1U, 4U, 0x11U) == SLE_TEAM_OK);

    assert(member.cfg.leader_id == 4U);
    assert(member.joined == 0U);
    assert(member.state == SLE_TEAM_NET_WAIT_POLICY);
    assert(member.last_hello_s == 0U);
    assert(member.last_heartbeat_s == 0U);
    assert(member.last_config_s == 0U);
    assert(member.last_leader_seen_s == 0U);
    assert(member.last_parent_seen_s == 0U);
    assert(member.upstream_parent_id == 0U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_WAIT_POLICY);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.cfg.relay_enabled == 0U);
    assert(member.cfg.relay_tier == 0U);
    assert(member.cfg.max_downstream == 0U);
    assert(member.cfg.relay_discovery_only == 0U);
    assert(test_decode_last_app_packet(&member_rt, &app) != 0U);
    assert(app.app_msg_type == SLE_TEAM_APP_HELLO);
    assert(app.src_id == 2U);
    assert(app.dst_id == 4U);
}

static void test_member_select_new_leader_can_rejoin_fresh_policy(void)
{
    test_runtime_t leader_rt = {.name = "new-leader", .now_s = 30U};
    test_runtime_t member_rt = {.name = "member", .now_s = 30U};
    sle_team_node_t leader;
    sle_team_node_t member;

    test_init_node(&leader, &leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.cfg.team_id = 1U;
    member.cfg.leader_id = 1U;
    member.joined = 1U;
    member.state = SLE_TEAM_NET_ONLINE;
    member.upstream_parent_id = 3U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    member.cfg.relay_allowed = 1U;
    member.cfg.relay_enabled = 1U;
    member.cfg.relay_tier = 1U;
    member.cfg.max_downstream = 7U;

    assert(sle_team_node_member_select_leader(&member, 1U, 4U, 0x11U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    test_deliver_last(&leader_rt, &member);

    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.state == SLE_TEAM_NET_ONLINE);
    assert(member.upstream_parent_id == 4U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_CONNECTED);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.cfg.relay_enabled == 0U);
    assert(sle_team_node_find_member(&leader, 2U) != NULL);
    assert(sle_team_node_find_member(&leader, 2U)->parent_id == 4U);
}

static void test_member_on_new_leader_ignores_stale_old_leader_policy(void)
{
    test_runtime_t old_leader_rt = {.name = "old-leader", .now_s = 31U};
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 31U};
    test_runtime_t member_rt = {.name = "member", .now_s = 31U};
    sle_team_node_t old_leader;
    sle_team_node_t new_leader;
    sle_team_node_t member;
    uint32_t new_leader_seen_s;

    test_init_node(&old_leader, &old_leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.cfg.leader_id = 1U;
    member.joined = 1U;
    member.state = SLE_TEAM_NET_ONLINE;
    member.upstream_parent_id = 1U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    assert(sle_team_node_member_select_leader(&member, 1U, 4U, 0x11U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &new_leader);
    test_deliver_last(&new_leader_rt, &member);
    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 4U);
    new_leader_seen_s = member.last_leader_seen_s;

    old_leader.members[0].member_id = 2U;
    old_leader.members[0].online = 1U;
    old_leader.members[0].parent_id = 1U;
    old_leader.members[0].next_hop_id = 1U;
    old_leader.members[0].relay_allowed = 1U;
    old_leader.members[0].relay_tier = 1U;
    old_leader.members[0].max_downstream = 7U;

    assert(sle_team_node_send_route_update(&old_leader, 2U, 1U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    old_leader_rt.last_tx_len = 0U;
    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 4U);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.last_leader_seen_s == new_leader_seen_s);

    assert(sle_team_node_send_config(&old_leader, 2U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 4U);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.last_leader_seen_s == new_leader_seen_s);
}

static void test_member_rejects_lower_term_old_leader_packets(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 33U};
    test_runtime_t member_rt = {.name = "member", .now_s = 33U};
    sle_team_node_t leader;
    sle_team_node_t member;
    uint32_t accepted_seen_s;

    test_init_node(&leader, &leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.leader_term = 2U;
    assert(sle_team_node_member_select_leader_term(&member, 1U, 4U, 0x11U, 2U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    test_deliver_last(&leader_rt, &member);
    assert(member.cfg.leader_id == 4U);
    assert(member.cfg.leader_term == 2U);
    assert(member.joined != 0U);
    accepted_seen_s = member.last_leader_seen_s;

    leader.cfg.leader_term = 1U;
    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 4U;
    leader.members[0].next_hop_id = 4U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    assert(sle_team_node_send_route_update(&leader, 2U, 4U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 4U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, leader_rt.last_tx, leader_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    leader_rt.last_tx_len = 0U;
    assert(member.cfg.leader_term == 2U);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.last_leader_seen_s == accepted_seen_s);

    assert(sle_team_node_send_config(&leader, 2U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, leader_rt.last_tx, leader_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    leader_rt.last_tx_len = 0U;
    assert(member.cfg.leader_term == 2U);
    assert(member.cfg.relay_allowed == 0U);
    assert(member.last_leader_seen_s == accepted_seen_s);

    assert(sle_team_node_send_heartbeat(&leader, 2U, 100U, SLE_TEAM_RSSI_UNKNOWN, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, leader_rt.last_tx, leader_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    assert(member.cfg.leader_term == 2U);
    assert(member.last_leader_seen_s == accepted_seen_s);
}

static void test_relay_rejects_forwarding_lower_term_old_leader_downlink(void)
{
    test_runtime_t old_leader_rt = {.name = "old-leader", .now_s = 34U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 34U};
    sle_team_node_t old_leader;
    sle_team_node_t relay;

    test_init_node(&old_leader, &old_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    old_leader.cfg.leader_term = 1U;
    relay.cfg.leader_id = 4U;
    relay.cfg.leader_term = 2U;
    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.upstream_parent_id = 4U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    relay.last_leader_seen_s = 34U;

    old_leader.members[0].member_id = 2U;
    old_leader.members[0].online = 1U;
    old_leader.members[0].parent_id = 3U;
    old_leader.members[0].next_hop_id = 3U;
    assert(sle_team_node_send_route_update(&old_leader, 2U, 3U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 3U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(relay_rt.last_tx_len == 0U);
    assert(relay.cfg.leader_term == 2U);
    assert(relay.last_leader_seen_s == 34U);
}

static void test_member_accepts_higher_term_new_leader_route(void)
{
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 35U};
    test_runtime_t old_member_rt = {.name = "old-member", .now_s = 35U};
    sle_team_node_t new_leader;
    sle_team_node_t old_member;
    const sle_team_member_record_t *record;

    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_member, &old_member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    new_leader.cfg.leader_term = 2U;
    new_leader.cfg.max_downstream = 1U;
    old_member.cfg.leader_id = 1U;
    old_member.cfg.leader_term = 1U;
    old_member.joined = 0U;
    old_member.state = SLE_TEAM_NET_WAIT_POLICY;
    old_member.upstream_parent_id = 1U;
    old_member.upstream_parent_state = SLE_TEAM_PARENT_WAIT_POLICY;
    old_member.cfg.relay_allowed = 1U;
    old_member.cfg.relay_enabled = 1U;
    old_member.cfg.relay_tier = 1U;
    old_member.cfg.max_downstream = 7U;

    new_leader.members[0].member_id = 3U;
    new_leader.members[0].online = 1U;
    new_leader.members[0].parent_id = 4U;
    new_leader.members[0].next_hop_id = 4U;
    new_leader.members[0].relay_allowed = 1U;
    new_leader.members[0].relay_tier = 1U;
    new_leader.members[0].max_downstream = 7U;
    new_leader.members[0].last_seen_s = 35U;
    new_leader.members[0].last_rssi_dbm = -45;
    new_leader.members[1].member_id = 2U;
    new_leader.members[1].online = 0U;
    new_leader.members[1].policy_pending = 1U;
    new_leader.members[1].parent_id = 3U;
    new_leader.members[1].next_hop_id = 3U;
    new_leader.members[1].last_seen_s = 35U;

    assert(sle_team_node_send_route_update(&new_leader, 2U, 3U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 3U) == SLE_TEAM_OK);
    test_deliver_last(&new_leader_rt, &old_member);

    assert(old_member.cfg.leader_id == 4U);
    assert(old_member.cfg.leader_term == 2U);
    assert(old_member.joined != 0U);
    assert(old_member.state == SLE_TEAM_NET_ONLINE);
    assert(old_member.upstream_parent_id == 3U);
    assert(old_member.cfg.relay_allowed == 0U);
    assert(old_member.cfg.relay_enabled == 0U);

    assert(sle_team_node_send_heartbeat(&old_member, old_member.cfg.leader_id, 90U, -48, 1U) == SLE_TEAM_OK);
    test_deliver_last(&old_member_rt, &new_leader);
    record = sle_team_node_find_member(&new_leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
}

static void test_member_rejects_non_higher_term_other_leader_route(void)
{
    test_runtime_t other_leader_rt = {.name = "other-leader", .now_s = 36U};
    test_runtime_t member_rt = {.name = "member", .now_s = 36U};
    sle_team_node_t other_leader;
    sle_team_node_t member;

    test_init_node(&other_leader, &other_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    member.cfg.leader_id = 1U;
    member.cfg.leader_term = 2U;
    member.joined = 1U;
    member.state = SLE_TEAM_NET_ONLINE;
    member.upstream_parent_id = 1U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    other_leader.cfg.leader_term = 2U;
    assert(sle_team_node_send_route_update(&other_leader, 2U, 4U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 4U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, other_leader_rt.last_tx, other_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(member.cfg.leader_id == 1U);
    assert(member.cfg.leader_term == 2U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 1U);
}

static void test_relay_forwards_stale_child_hello_to_current_leader(void)
{
    test_runtime_t old_member_rt = {.name = "old-member", .now_s = 37U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 37U};
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 37U};
    sle_team_node_t old_member;
    sle_team_node_t relay;
    sle_team_node_t new_leader;
    sle_team_app_packet_t app;
    const sle_team_member_record_t *record;

    test_init_node(&old_member, &old_member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    old_member.cfg.leader_id = 1U;
    old_member.cfg.leader_term = 1U;
    relay.cfg.leader_id = 4U;
    relay.cfg.leader_term = 2U;
    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.upstream_parent_id = 4U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    new_leader.cfg.leader_term = 2U;

    assert(sle_team_node_send_hello(&old_member, old_member.cfg.leader_id) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, old_member_rt.last_tx, old_member_rt.last_tx_len) == SLE_TEAM_OK);
    assert(test_decode_last_app_packet(&relay_rt, &app) != 0U);
    assert(app.app_msg_type == SLE_TEAM_APP_HELLO);
    assert(app.src_id == 2U);
    assert(app.dst_id == 4U);
    assert(app.leader_term == 1U);

    test_deliver_last(&relay_rt, &new_leader);
    record = sle_team_node_find_member(&new_leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 4U);
    assert(record->relay_allowed == 0U);
}

static void test_old_member_rejoins_higher_term_leader_through_current_relay(void)
{
    test_runtime_t old_member_rt = {.name = "old-member", .now_s = 38U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 38U};
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 38U};
    test_runtime_t old_leader_rt = {.name = "old-leader", .now_s = 38U};
    sle_team_node_t old_member;
    sle_team_node_t relay;
    sle_team_node_t new_leader;
    sle_team_node_t old_leader;
    sle_team_app_packet_t forwarded;
    const sle_team_member_record_t *record;

    test_init_node(&old_member, &old_member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_leader, &old_leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    old_member.cfg.leader_id = 1U;
    old_member.cfg.leader_term = 1U;
    old_member.joined = 0U;
    old_member.state = SLE_TEAM_NET_WAIT_POLICY;
    old_member.cfg.relay_allowed = 1U;
    old_member.cfg.relay_enabled = 1U;
    relay.cfg.leader_id = 4U;
    relay.cfg.leader_term = 2U;
    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.upstream_parent_id = 4U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    new_leader.cfg.leader_term = 2U;
    new_leader.cfg.max_downstream = 1U;
    new_leader.members[0].member_id = 3U;
    new_leader.members[0].online = 1U;
    new_leader.members[0].relay_allowed = 1U;
    new_leader.members[0].relay_tier = 1U;
    new_leader.members[0].max_downstream = 7U;
    new_leader.members[0].parent_id = 4U;
    new_leader.members[0].next_hop_id = 4U;
    new_leader.members[0].last_seen_s = 38U;

    assert(sle_team_node_send_hello(&old_member, old_member.cfg.leader_id) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, old_member_rt.last_tx, old_member_rt.last_tx_len) == SLE_TEAM_OK);
    assert(test_decode_last_app_packet(&relay_rt, &forwarded) != 0U);
    assert(forwarded.src_id == 2U);
    assert(forwarded.dst_id == 4U);
    assert(forwarded.leader_term == 1U);
    test_deliver_last(&relay_rt, &new_leader);

    record = sle_team_node_find_member(&new_leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_route_update(&new_leader, 2U, record->parent_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, record->next_hop_id) == SLE_TEAM_OK);
    test_deliver_last(&new_leader_rt, &old_member);
    assert(old_member.cfg.leader_id == 4U);
    assert(old_member.cfg.leader_term == 2U);
    assert(old_member.joined != 0U);
    assert(old_member.upstream_parent_id == 3U);
    assert(old_member.cfg.relay_allowed == 0U);

    old_leader.cfg.leader_term = 1U;
    old_leader.members[0].member_id = 2U;
    old_leader.members[0].online = 1U;
    old_leader.members[0].parent_id = 1U;
    old_leader.members[0].next_hop_id = 1U;
    old_leader.members[0].relay_allowed = 1U;
    assert(sle_team_node_send_route_update(&old_leader, 2U, 1U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&old_member, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(old_member.cfg.leader_id == 4U);
    assert(old_member.cfg.leader_term == 2U);
    assert(old_member.upstream_parent_id == 3U);
}

static void test_member_select_new_leader_joins_current_relay_when_direct_full(void)
{
    test_runtime_t old_leader_rt = {.name = "old-leader", .now_s = 32U};
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 32U};
    test_runtime_t member_rt = {.name = "member", .now_s = 32U};
    sle_team_node_t old_leader;
    sle_team_node_t new_leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;
    const sle_team_member_record_t *relay;

    test_init_node(&old_leader, &old_leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    new_leader.cfg.max_downstream = 1U;

    new_leader.members[0].member_id = 3U;
    new_leader.members[0].online = 1U;
    new_leader.members[0].parent_id = 4U;
    new_leader.members[0].next_hop_id = 4U;
    new_leader.members[0].relay_allowed = 1U;
    new_leader.members[0].relay_tier = 1U;
    new_leader.members[0].max_downstream = 7U;
    new_leader.members[0].last_seen_s = 32U;
    new_leader.members[0].last_rssi_dbm = -50;

    member.cfg.leader_id = 1U;
    member.joined = 1U;
    member.state = SLE_TEAM_NET_ONLINE;
    member.upstream_parent_id = 1U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    member.cfg.relay_allowed = 1U;
    member.cfg.relay_enabled = 1U;
    member.cfg.relay_tier = 1U;
    member.cfg.max_downstream = 7U;

    assert(sle_team_node_member_select_leader(&member, 1U, 4U, 0x11U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &new_leader);

    record = sle_team_node_find_member(&new_leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_route_update(&new_leader, 2U, record->parent_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, record->next_hop_id) == SLE_TEAM_OK);
    test_deliver_last(&new_leader_rt, &member);
    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 3U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_CONNECTED);
    assert(member.cfg.relay_allowed == 0U);

    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -52, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &new_leader);
    record = sle_team_node_find_member(&new_leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    relay = sle_team_node_find_member(&new_leader, 3U);
    assert(relay != NULL);
    assert(relay->relay_allowed != 0U);
    assert(relay->child_count == 1U);

    old_leader.members[0].member_id = 2U;
    old_leader.members[0].online = 1U;
    old_leader.members[0].parent_id = 1U;
    old_leader.members[0].next_hop_id = 1U;
    old_leader.members[0].relay_allowed = 1U;
    old_leader.members[0].relay_tier = 1U;
    old_leader.members[0].max_downstream = 7U;
    assert(sle_team_node_send_route_update(&old_leader, 2U, 1U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&member, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(member.cfg.leader_id == 4U);
    assert(member.joined != 0U);
    assert(member.upstream_parent_id == 3U);
    assert(member.cfg.relay_allowed == 0U);
}

static void test_direct_member_offline_releases_slot_for_rejoin(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t member2_rt = {.name = "member2", .now_s = 1U};
    test_runtime_t member3_rt = {.name = "member3", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t member2;
    sle_team_node_t member3;
    const sle_team_member_record_t *record2;
    const sle_team_member_record_t *record3;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member2, &member2_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member3, &member3_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    assert(sle_team_node_send_hello(&member2, member2.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);
    assert(sle_team_node_send_heartbeat(&member2, member2.cfg.leader_id, 88U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);

    assert(sle_team_node_member_offline(&leader, 2U) == SLE_TEAM_OK);
    record2 = sle_team_node_find_member(&leader, 2U);
    assert(record2 != NULL);
    assert(record2->online == 0U);
    assert(record2->policy_pending == 0U);
    assert(record2->parent_id == 0U);

    assert(sle_team_node_send_hello(&member3, member3.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member3_rt, &leader);
    assert(sle_team_node_send_heartbeat(&member3, member3.cfg.leader_id, 86U, -36, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member3_rt, &leader);
    record3 = sle_team_node_find_member(&leader, 3U);
    assert(record3 != NULL);
    assert(record3->online != 0U);
    assert(record3->parent_id == 1U);
}

static void test_direct_capacity_and_relay_overflow_policy(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    sle_team_node_t leader;
    const sle_team_member_record_t *relay;
    const sle_team_member_record_t *overflow;
    uint8_t id;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);

    for (id = 2U; id <= 8U; id++) {
        assert(sle_team_node_pairing_approve(&leader, id) == SLE_TEAM_OK);
        test_confirm_member_by_heartbeat(&leader, id, leader_rt.now_s);
        assert(sle_team_node_find_member(&leader, id)->parent_id == 1U);
    }

    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);
    assert(sle_team_node_pairing_approve(&leader, 9U) == SLE_TEAM_OK);
    test_confirm_member_by_heartbeat(&leader, 9U, leader_rt.now_s);
    overflow = sle_team_node_find_member(&leader, 9U);
    assert(overflow != NULL);
    assert(overflow->parent_id != 0U);
    assert(overflow->parent_id != 1U);

    relay = sle_team_node_find_member(&leader, overflow->parent_id);
    assert(relay != NULL);
    assert(relay->relay_allowed != 0U);
    assert(relay->parent_id == 1U);
    assert(relay->child_count == 1U);
}

static void test_direct_capacity_auto_grants_first_relay_for_overflow(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t overflow_rt = {.name = "overflow", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t overflow;
    const sle_team_member_record_t *relay;
    const sle_team_member_record_t *overflow_record;
    uint8_t id;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&overflow, &overflow_rt, 9U, SLE_TEAM_ROLE_MEMBER);

    for (id = 2U; id <= 8U; id++) {
        assert(sle_team_node_pairing_approve(&leader, id) == SLE_TEAM_OK);
        test_confirm_member_by_heartbeat(&leader, id, leader_rt.now_s);
        assert(sle_team_node_find_member(&leader, id)->parent_id == 1U);
    }

    assert(sle_team_node_pairing_approve(&leader, 9U) == SLE_TEAM_OK);
    assert(sle_team_node_send_hello(&overflow, overflow.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&overflow_rt, &leader);

    overflow_record = sle_team_node_find_member(&leader, 9U);
    assert(overflow_record != NULL);
    assert(overflow_record->policy_pending != 0U);
    assert(overflow_record->parent_id != 0U);
    assert(overflow_record->parent_id != 1U);

    relay = sle_team_node_find_member(&leader, overflow_record->parent_id);
    assert(relay != NULL);
    assert(relay->online != 0U);
    assert(relay->relay_allowed != 0U);
    assert(relay->parent_id == 1U);
}

static void test_forwarded_initial_hello_uses_ingress_relay_parent(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t child_rt = {.name = "child", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t child;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&child, &child_rt, 9U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 3U;

    assert(sle_team_node_pairing_approve(&leader, 2U) == SLE_TEAM_OK);
    test_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);
    assert(sle_team_node_add_allowed_member(&leader, 9U) == SLE_TEAM_OK);
    leader_rt.last_tx_len = 0U;

    leader.rx_ingress_relay_id = 2U;
    assert(sle_team_node_send_hello(&child, child.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&child_rt, &leader);
    leader.rx_ingress_relay_id = 0U;

    record = sle_team_node_find_member(&leader, 9U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 2U);
    assert(record->next_hop_id == 2U);
    assert(record->online == 0U);
}

static void test_overflow_promotes_second_relay_when_existing_relay_is_full(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    test_runtime_t relay1_rt = {.name = "relay1", .now_s = 1U};
    test_runtime_t relay2_rt = {.name = "relay2", .now_s = 1U};
    test_runtime_t overflow_rt = {.name = "overflow", .now_s = 1U};
    sle_team_node_t leader;
    sle_team_node_t relay1;
    sle_team_node_t relay2;
    sle_team_node_t overflow;
    const sle_team_member_record_t *record;
    uint8_t id;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay1, &relay1_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&relay2, &relay2_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&overflow, &overflow_rt, 11U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 2U;

    assert(sle_team_node_pairing_approve(&leader, 2U) == SLE_TEAM_OK);
    test_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    assert(sle_team_node_pairing_approve(&leader, 3U) == SLE_TEAM_OK);
    test_confirm_member_by_heartbeat(&leader, 3U, leader_rt.now_s);
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);

    for (id = 4U; id <= 10U; id++) {
        assert(sle_team_node_pairing_approve(&leader, id) == SLE_TEAM_OK);
        test_confirm_member_by_heartbeat(&leader, id, leader_rt.now_s);
        record = sle_team_node_find_member(&leader, id);
        assert(record != NULL);
        assert(record->parent_id == 2U);
    }

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->relay_allowed != 0U);
    assert(record->child_count == 7U);

    assert(sle_team_node_pairing_approve(&leader, 11U) == SLE_TEAM_OK);
    assert(sle_team_node_send_hello(&overflow, overflow.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&overflow_rt, &leader);

    record = sle_team_node_find_member(&leader, 11U);
    assert(record != NULL);
    assert(record->parent_id == 3U);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->relay_allowed != 0U);
    assert(record->parent_id == 1U);
}

static void test_hello_waits_for_heartbeat_and_reserves_direct_slot(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 10U};
    test_runtime_t member2_rt = {.name = "member2", .now_s = 10U};
    test_runtime_t member3_rt = {.name = "member3", .now_s = 10U};
    sle_team_node_t leader;
    sle_team_node_t member2;
    sle_team_node_t member3;
    const sle_team_member_record_t *record2;
    const sle_team_member_record_t *record3;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member2, &member2_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member3, &member3_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    assert(sle_team_node_send_hello(&member2, member2.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);
    record2 = sle_team_node_find_member(&leader, 2U);
    assert(record2 != NULL);
    assert(record2->online == 0U);
    assert(record2->policy_pending != 0U);
    assert(record2->parent_id == 1U);
    assert(leader_rt.joined_count == 0U);

    assert(sle_team_node_send_hello(&member3, member3.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member3_rt, &leader);
    record3 = sle_team_node_find_member(&leader, 3U);
    assert(record3 != NULL);
    assert(record3->online == 0U);
    assert(record3->policy_pending == 0U);
    assert(record3->parent_id == 0U);

    assert(sle_team_node_send_heartbeat(&member2, member2.cfg.leader_id, 88U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);
    record2 = sle_team_node_find_member(&leader, 2U);
    assert(record2 != NULL);
    assert(record2->online != 0U);
    assert(record2->policy_pending == 0U);
    assert(leader_rt.joined_count == 1U);

    assert(sle_team_node_send_heartbeat(&member2, member2.cfg.leader_id, 88U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);
    assert(leader_rt.joined_count == 1U);
}

static void test_repeated_hello_keeps_stable_member_parent(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 20U};
    test_runtime_t member_rt = {.name = "member", .now_s = 20U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member, &member_rt, 3U, SLE_TEAM_ROLE_MEMBER);

    leader.cfg.max_downstream = 7U;
    test_join_member(&leader, &leader_rt, &relay, &relay_rt);
    assert(sle_team_node_send_heartbeat(&relay, relay.cfg.leader_id, 90U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&relay_rt, &leader);
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);

    test_join_member(&leader, &leader_rt, &member, &member_rt);
    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->parent_id == 1U);

    leader.cfg.max_downstream = 1U;
    member_rt.now_s = 21U;
    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 1U);
    assert(leader_rt.joined_count == 2U);
}

static void test_repeated_hello_reassigns_pending_member_to_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 20U};
    test_runtime_t member_rt = {.name = "member", .now_s = 20U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member, &member_rt, 3U, SLE_TEAM_ROLE_MEMBER);

    leader.cfg.max_downstream = 2U;
    test_join_member(&leader, &leader_rt, &relay, &relay_rt);
    assert(sle_team_node_send_heartbeat(&relay, relay.cfg.leader_id, 90U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&relay_rt, &leader);
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 1U);

    leader.cfg.max_downstream = 1U;
    member_rt.now_s = 21U;
    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 2U);
    assert(record->next_hop_id == 2U);
}

static void test_repeated_hello_reassigns_pending_member_to_direct_but_keeps_relay_delivery_hop(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 20U};
    test_runtime_t member_rt = {.name = "member", .now_s = 20U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member, &member_rt, 3U, SLE_TEAM_ROLE_MEMBER);

    leader.cfg.max_downstream = 1U;
    test_join_member(&leader, &leader_rt, &relay, &relay_rt);
    assert(sle_team_node_send_heartbeat(&relay, relay.cfg.leader_id, 90U, -35, 1U) == SLE_TEAM_OK);
    test_deliver_last(&relay_rt, &leader);
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 2U);
    assert(record->next_hop_id == 2U);

    leader.cfg.max_downstream = 2U;
    member_rt.now_s = 21U;
    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 1U);
    assert(record->next_hop_id == 2U);
}

static void test_connected_route_update_rejoins_member_without_hello_ack(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 12U};
    test_runtime_t member_rt = {.name = "member", .now_s = 12U};
    sle_team_node_t leader;
    sle_team_node_t member;
    sle_team_app_packet_t app;
    sle_team_ack_body_t ack;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    member.joined = 0U;
    member.state = SLE_TEAM_NET_WAIT_POLICY;
    member.upstream_parent_id = 1U;
    member.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    assert(sle_team_node_send_route_update(&leader, 3U, 2U, (uint8_t)SLE_TEAM_PARENT_CONNECTED, 2U) == SLE_TEAM_OK);
    test_deliver_last(&leader_rt, &member);

    assert(member.joined != 0U);
    assert(member.state == SLE_TEAM_NET_ONLINE);
    assert(member.upstream_parent_id == 2U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_CONNECTED);
    assert(member.last_parent_seen_s == member_rt.now_s);
    assert(member_rt.joined_count == 1U);
    assert(test_decode_last_app_packet(&member_rt, &app) != 0U);
    assert(app.app_msg_type == SLE_TEAM_APP_ACK);
    assert(app.dst_id == member.cfg.leader_id);
    (void)memcpy(&ack, app.body, sizeof(ack));
    assert(ack.acked_msg_type == SLE_TEAM_APP_ROUTE_UPDATE);
    assert(ack.status_code == 0U);
}

static void test_ack_confirms_only_matching_pending_policy(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 12U};
    test_runtime_t member_rt = {.name = "member", .now_s = 12U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->online == 0U);

    test_send_member_ack(&member, &member_rt, (uint16_t)(record->pending_ack_seq + 1U), SLE_TEAM_APP_HELLO);
    assert(sle_team_node_on_packet(&leader, member_rt.last_tx, member_rt.last_tx_len) == SLE_TEAM_ERR_UNSUPPORTED);
    member_rt.last_tx_len = 0U;
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->online == 0U);
    assert(leader_rt.joined_count == 0U);

    test_send_member_ack(&member, &member_rt, record->pending_ack_seq, SLE_TEAM_APP_HELLO);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending == 0U);
    assert(record->online != 0U);
    assert(leader_rt.joined_count == 1U);
}

static void test_route_update_ack_confirms_pending_policy(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 12U};
    test_runtime_t member_rt = {.name = "member", .now_s = 12U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    leader.next_seq = 77U;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->pending_ack_seq == 77U);

    test_send_member_ack(&member, &member_rt, record->pending_ack_seq, SLE_TEAM_APP_ROUTE_UPDATE);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending == 0U);
    assert(record->online != 0U);
}

static void test_pending_policy_send_failure_keeps_previous_ack_target(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 12U};
    test_runtime_t member_rt = {.name = "member", .now_s = 12U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;
    uint16_t pending_ack_seq;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    pending_ack_seq = record->pending_ack_seq;

    leader_rt.fail_next_tx = 1U;
    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&leader, member_rt.last_tx, member_rt.last_tx_len) != SLE_TEAM_OK);
    member_rt.last_tx_len = 0U;
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->pending_ack_seq == pending_ack_seq);
}

static void test_pending_policy_timeout_releases_direct_slot(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 10U};
    test_runtime_t member2_rt = {.name = "member2", .now_s = 10U};
    test_runtime_t member3_rt = {.name = "member3", .now_s = 15U};
    sle_team_node_t leader;
    sle_team_node_t member2;
    sle_team_node_t member3;
    const sle_team_member_record_t *record2;
    const sle_team_member_record_t *record3;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member2, &member2_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&member3, &member3_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    assert(sle_team_node_send_hello(&member2, member2.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member2_rt, &leader);
    record2 = sle_team_node_find_member(&leader, 2U);
    assert(record2 != NULL);
    assert(record2->policy_pending != 0U);

    leader_rt.now_s = 14U;
    sle_team_node_tick(&leader);
    record2 = sle_team_node_find_member(&leader, 2U);
    assert(record2 == NULL);

    assert(sle_team_node_send_hello(&member3, member3.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member3_rt, &leader);
    record3 = sle_team_node_find_member(&leader, 3U);
    assert(record3 != NULL);
    assert(record3->policy_pending != 0U);
    assert(record3->parent_id == 1U);
}

static void test_pending_relay_child_reserves_relay_capacity(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t child3_rt = {.name = "child3", .now_s = 20U};
    test_runtime_t child4_rt = {.name = "child4", .now_s = 20U};
    sle_team_node_t leader;
    sle_team_node_t child3;
    sle_team_node_t child4;
    const sle_team_member_record_t *relay;
    const sle_team_member_record_t *child3_record;
    const sle_team_member_record_t *child4_record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&child3, &child3_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&child4, &child4_rt, 4U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;
    leader.members[0].member_id = 2U;
    leader.members[0].role = SLE_TEAM_ROLE_MEMBER;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 1U;
    leader.members[0].last_seen_s = leader_rt.now_s;

    assert(sle_team_node_send_hello(&child3, child3.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&child3_rt, &leader);
    child3_record = sle_team_node_find_member(&leader, 3U);
    relay = sle_team_node_find_member(&leader, 2U);
    assert(child3_record != NULL);
    assert(relay != NULL);
    assert(child3_record->online == 0U);
    assert(child3_record->policy_pending != 0U);
    assert(child3_record->parent_id == 2U);
    assert(relay->child_count == 1U);

    assert(sle_team_node_send_hello(&child4, child4.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&child4_rt, &leader);
    child4_record = sle_team_node_find_member(&leader, 4U);
    assert(child4_record != NULL);
    assert(child4_record->online == 0U);
    assert(child4_record->policy_pending == 0U);
    assert(child4_record->parent_id == 0U);
}

static void test_online_direct_member_can_be_granted_relay_before_overflow(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    sle_team_node_t leader;
    const sle_team_member_record_t *relay;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.cfg.max_downstream = 1U;

    assert(sle_team_node_pairing_approve(&leader, 2U) == SLE_TEAM_OK);
    test_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    relay = sle_team_node_find_member(&leader, 2U);
    assert(relay != NULL);
    assert(relay->online != 0U);
    assert(relay->parent_id == 1U);
    assert(relay->relay_allowed == 0U);

    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);
    relay = sle_team_node_find_member(&leader, 2U);
    assert(relay != NULL);
    assert(relay->online != 0U);
    assert(relay->parent_id == 1U);
    assert(relay->relay_allowed != 0U);
    assert(relay->relay_tier == 1U);
    assert(relay->max_downstream == 7U);
}

static void test_pairing_approve_respects_pending_confirmation(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 8U};
    sle_team_node_t leader;
    const sle_team_member_record_t *member;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    assert(sle_team_node_pairing_approve(&leader, 2U) == SLE_TEAM_OK);
    member = sle_team_node_find_member(&leader, 2U);
    assert(member != NULL);
    assert(member->online == 0U);
    assert(member->policy_pending != 0U);
    assert(leader_rt.joined_count == 0U);

    test_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    member = sle_team_node_find_member(&leader, 2U);
    assert(member != NULL);
    assert(member->online != 0U);
    assert(member->policy_pending == 0U);
    assert(leader_rt.joined_count == 1U);
}

static void test_relay_forward_decrements_ttl(void)
{
    test_runtime_t relay_rt = {.name = "relay", .now_s = 1U};
    test_runtime_t child_rt = {.name = "child", .now_s = 1U};
    sle_team_node_t relay;
    sle_team_node_t child;
    sle_team_pos_body_t pos;
    sle_team_app_packet_t forwarded;

    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&child, &child_rt, 9U, SLE_TEAM_ROLE_MEMBER);
    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    relay.upstream_parent_id = 1U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    child.joined = 1U;
    child.state = SLE_TEAM_NET_ONLINE;
    child.upstream_parent_id = 3U;
    child.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    (void)memset(&pos, 0, sizeof(pos));
    assert(sle_team_node_send_position(&child, 1U, &pos) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, child_rt.last_tx, child_rt.last_tx_len) == SLE_TEAM_OK);
    assert(test_decode_last_app_packet(&relay_rt, &forwarded) != 0U);
    assert(forwarded.src_id == 9U);
    assert(forwarded.dst_id == 1U);
    assert(forwarded.ttl == 3U);
}

static void test_relay_leader_downlink_refreshes_parent_liveness(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 20U};
    test_runtime_t peer_rt = {.name = "peer", .now_s = 21U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t peer;
    sle_team_app_packet_t forwarded;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&peer, &peer_rt, 4U, SLE_TEAM_ROLE_MEMBER);
    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    relay.upstream_parent_id = 1U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    relay.last_leader_seen_s = 5U;
    relay.last_parent_seen_s = 5U;

    assert(sle_team_node_send_heartbeat(&leader, 9U, 100U, -40, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, leader_rt.last_tx, leader_rt.last_tx_len) == SLE_TEAM_OK);

    assert(relay.last_leader_seen_s == 20U);
    assert(relay.last_parent_seen_s == 20U);
    assert(test_decode_last_app_packet(&relay_rt, &forwarded) != 0U);
    assert(forwarded.src_id == 1U);
    assert(forwarded.dst_id == 9U);
    assert(forwarded.ttl == 3U);

    relay.last_leader_seen_s = 20U;
    relay.last_parent_seen_s = 20U;
    assert(sle_team_node_send_position(&peer, 1U, &(sle_team_pos_body_t){0}) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, peer_rt.last_tx, peer_rt.last_tx_len) == SLE_TEAM_OK);
    assert(relay.last_leader_seen_s == 20U);
    assert(relay.last_parent_seen_s == 20U);
}

static void test_relay_timeout_marks_children_offline_without_callback(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    sle_team_node_t leader;
    const sle_team_member_record_t *relay;
    const sle_team_member_record_t *child;
    const sle_team_member_record_t *direct;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.ops.on_relay_offline = NULL;
    leader.cfg.heartbeat_interval_s = 0U;

    leader.members[0].member_id = 3U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].last_seen_s = 1U;
    leader.members[1].member_id = 9U;
    leader.members[1].online = 0U;
    leader.members[1].policy_pending = 1U;
    leader.members[1].parent_id = 3U;
    leader.members[1].last_seen_s = leader_rt.now_s;
    leader.members[2].member_id = 2U;
    leader.members[2].online = 1U;
    leader.members[2].parent_id = 1U;
    leader.members[2].last_seen_s = leader_rt.now_s;

    child = sle_team_node_find_member(&leader, 9U);
    assert(child != NULL);
    relay = sle_team_node_find_member(&leader, 3U);
    assert(relay != NULL);
    assert(relay->relay_allowed != 0U);
    direct = sle_team_node_find_member(&leader, 2U);
    assert(direct != NULL);
    assert(direct->parent_id == 1U);
    assert(direct->online != 0U);

    sle_team_node_tick(&leader);

    relay = sle_team_node_find_member(&leader, 3U);
    child = sle_team_node_find_member(&leader, 9U);
    direct = sle_team_node_find_member(&leader, 2U);
    assert(relay != NULL);
    assert(child != NULL);
    assert(direct != NULL);
    assert(relay->online == 0U);
    assert(child->online == 0U);
    assert(child->policy_pending == 0U);
    assert(direct->online != 0U);
    assert(leader_rt.last_tx_len == 0U);
    assert(leader_rt.relay_offline_count == 0U);
}

static void test_relay_loss_selects_replacement_child_and_old_relay_rejoins_as_child(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    test_runtime_t old_relay_rt = {.name = "old-relay", .now_s = 21U};
    test_runtime_t child3_rt = {.name = "child3", .now_s = 22U};
    sle_team_node_t leader;
    sle_team_node_t old_relay;
    sle_team_node_t child3;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_relay, &old_relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&child3, &child3_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;
    leader.cfg.heartbeat_interval_s = 0U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 1U;
    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 2U;
    leader.members[1].next_hop_id = 2U;
    leader.members[1].last_seen_s = 19U;
    leader.members[2].member_id = 4U;
    leader.members[2].online = 1U;
    leader.members[2].parent_id = 2U;
    leader.members[2].next_hop_id = 2U;
    leader.members[2].last_seen_s = 18U;

    sle_team_node_tick(&leader);

    assert(leader.relay_recovery_pending != 0U);
    assert(leader.relay_recovery_lost_relay_id == 2U);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->relay_recovery_candidate != 0U);
    record = sle_team_node_find_member(&leader, 4U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->relay_recovery_candidate != 0U);

    assert(sle_team_node_send_hello(&old_relay, old_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 0U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_hello(&child3, child3.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&child3_rt, &leader);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 1U);
    assert(record->relay_allowed != 0U);
    assert(record->relay_recovery_candidate != 0U);

    test_confirm_member_by_heartbeat(&leader, 3U, leader_rt.now_s);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->relay_allowed != 0U);
    assert(record->relay_recovery_candidate == 0U);
    assert(leader.relay_recovery_pending != 0U);
    assert(leader.relay_recovery_lost_relay_id == 2U);

    assert(sle_team_node_send_hello(&old_relay, old_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->relay_allowed == 0U);
}

static void test_fast_old_relay_hello_after_recovery_rejoins_as_child(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 40U};
    test_runtime_t old_relay_rt = {.name = "old-relay", .now_s = 40U};
    sle_team_node_t leader;
    sle_team_node_t old_relay;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_relay, &old_relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 3U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[2].member_id = 4U;
    leader.members[2].online = 1U;
    leader.members[2].parent_id = 1U;
    leader.members[2].next_hop_id = 1U;
    leader.members[3].member_id = 5U;
    leader.members[3].online = 1U;
    leader.members[3].parent_id = 1U;
    leader.members[3].next_hop_id = 1U;
    leader.members[4].member_id = 9U;
    leader.members[4].online = 1U;
    leader.members[4].parent_id = 1U;
    leader.members[4].next_hop_id = 1U;
    leader.members[4].relay_allowed = 1U;
    leader.members[4].relay_tier = 1U;
    leader.members[4].max_downstream = 7U;
    leader.relay_recovery_lost_relay_id = 2U;

    assert(sle_team_node_send_hello(&old_relay, old_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 9U);
    assert(record->next_hop_id == 9U);
    assert(record->relay_allowed == 0U);
}

static void test_relay_recovery_keeps_remaining_candidates_on_replacement_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 50U};
    test_runtime_t selected_relay_rt = {.name = "selected-relay", .now_s = 50U};
    test_runtime_t remaining_child_rt = {.name = "remaining-child", .now_s = 50U};
    sle_team_node_t leader;
    sle_team_node_t selected_relay;
    sle_team_node_t remaining_child;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&selected_relay, &selected_relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&remaining_child, &remaining_child_rt, 4U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 7U;

    leader.members[0].member_id = 3U;
    leader.members[0].online = 0U;
    leader.members[0].parent_id = 0U;
    leader.members[0].relay_recovery_candidate = 1U;
    leader.members[0].last_seen_s = 49U;
    leader.members[1].member_id = 4U;
    leader.members[1].online = 0U;
    leader.members[1].parent_id = 0U;
    leader.members[1].relay_recovery_candidate = 1U;
    leader.members[1].last_seen_s = 48U;
    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 2U;
    leader.relay_recovery_selected_id = 3U;

    assert(sle_team_node_send_hello(&selected_relay, selected_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&selected_relay_rt, &leader);
    test_confirm_member_by_heartbeat(&leader, 3U, leader_rt.now_s);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(leader.relay_recovery_pending != 0U);
    assert(leader.relay_recovery_selected_id == 3U);
    assert(record->relay_recovery_candidate == 0U);

    assert(sle_team_node_send_hello(&remaining_child, remaining_child.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&remaining_child_rt, &leader);

    record = sle_team_node_find_member(&leader, 4U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_recovery_candidate != 0U);
    assert(leader.relay_recovery_pending != 0U);

    test_confirm_member_by_heartbeat(&leader, 4U, leader_rt.now_s);

    record = sle_team_node_find_member(&leader, 4U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_recovery_candidate == 0U);
    assert(leader.relay_recovery_pending == 0U);
}

static void test_selected_recovery_relay_preserves_live_next_hop(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 55U};
    test_runtime_t selected_rt = {.name = "selected", .now_s = 55U};
    sle_team_node_t leader;
    sle_team_node_t selected;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&selected, &selected_rt, 5U, SLE_TEAM_ROLE_MEMBER);
    leader.members[0].member_id = 3U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 55U;
    leader.members[1].member_id = 5U;
    leader.members[1].online = 0U;
    leader.members[1].next_hop_id = 3U;
    leader.members[1].relay_recovery_candidate = 1U;
    leader.members[1].last_seen_s = 54U;
    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 2U;
    leader.relay_recovery_selected_id = 5U;

    assert(sle_team_node_send_heartbeat(&selected, selected.cfg.leader_id, 90U, -39, 1U) == SLE_TEAM_OK);
    test_deliver_last(&selected_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->relay_allowed != 0U);
    assert(record->parent_id == 1U);
    assert(record->next_hop_id == 3U);

    test_send_member_ack(&selected, &selected_rt, record->pending_ack_seq, SLE_TEAM_APP_ROUTE_UPDATE);
    test_deliver_last(&selected_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->relay_allowed != 0U);
    assert(record->next_hop_id == 3U);
}

static void test_relay_recovery_heartbeat_reassigns_stale_records(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 60U};
    test_runtime_t old_relay_rt = {.name = "old-relay", .now_s = 60U};
    test_runtime_t stale_child_rt = {.name = "stale-child", .now_s = 60U};
    sle_team_node_t leader;
    sle_team_node_t old_relay;
    sle_team_node_t stale_child;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_relay, &old_relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&stale_child, &stale_child_rt, 5U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 7U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 4U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 59U;
    leader.members[1].member_id = 4U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].last_seen_s = 60U;
    leader.members[2].member_id = 5U;
    leader.members[2].online = 0U;
    leader.members[2].policy_pending = 1U;
    leader.members[2].parent_id = 2U;
    leader.members[2].next_hop_id = 4U;
    leader.members[2].relay_recovery_candidate = 1U;
    leader.members[2].last_seen_s = 59U;
    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 2U;
    leader.relay_recovery_selected_id = 4U;

    assert(sle_team_node_send_heartbeat(&old_relay, old_relay.cfg.leader_id, 90U, -44, 1U) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_tier == 0U);
    assert(record->max_downstream == 0U);

    assert(sle_team_node_send_heartbeat(&old_relay, old_relay.cfg.leader_id, 90U, -44, 1U) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_tier == 0U);
    assert(record->max_downstream == 0U);

    assert(sle_team_node_send_heartbeat(&stale_child, stale_child.cfg.leader_id, 90U, -42, 1U) == SLE_TEAM_OK);
    test_deliver_last(&stale_child_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_recovery_candidate != 0U);

    assert(sle_team_node_send_heartbeat(&stale_child, stale_child.cfg.leader_id, 90U, -42, 1U) == SLE_TEAM_OK);
    test_deliver_last(&stale_child_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_recovery_candidate == 0U);
    assert(leader.relay_recovery_pending == 0U);
    assert(leader.relay_recovery_lost_relay_id == 2U);

    assert(sle_team_node_send_hello(&old_relay, old_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);

    test_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
    assert(leader.relay_recovery_lost_relay_id == 2U);
}

static void test_pending_relay_child_position_confirms_through_current_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 70U};
    test_runtime_t relay_rt = {.name = "relay", .now_s = 70U};
    test_runtime_t child_rt = {.name = "child", .now_s = 70U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t child;
    sle_team_pos_body_t pos;
    sle_team_app_packet_t forwarded;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&relay, &relay_rt, 3U, SLE_TEAM_ROLE_MEMBER);
    test_init_node(&child, &child_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 7U;
    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 4U;

    child.joined = 1U;
    child.state = SLE_TEAM_NET_ONLINE;
    child.upstream_parent_id = 3U;
    child.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    relay.joined = 1U;
    relay.state = SLE_TEAM_NET_ONLINE;
    relay.cfg.relay_allowed = 1U;
    relay.cfg.relay_enabled = 1U;
    relay.upstream_parent_id = 1U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 0U;
    leader.members[0].policy_pending = 1U;
    leader.members[0].parent_id = 3U;
    leader.members[0].next_hop_id = 3U;
    leader.members[0].relay_recovery_candidate = 1U;
    leader.members[0].last_seen_s = 69U;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].last_seen_s = 70U;

    (void)memset(&pos, 0, sizeof(pos));
    pos.latitude_e6 = 39908456;
    pos.longitude_e6 = 116397128;
    pos.speed_cms = 120U;
    pos.heading_deg = 90U;
    pos.battery_percent = 87U;
    pos.fix_status = 1U;
    pos.sat_count = 9U;
    assert(sle_team_node_send_position(&child, child.cfg.leader_id, &pos) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&relay, child_rt.last_tx, child_rt.last_tx_len) == SLE_TEAM_OK);
    assert(test_decode_last_app_packet(&relay_rt, &forwarded) != 0U);
    assert(forwarded.src_id == 2U);
    assert(forwarded.dst_id == 1U);
    assert(forwarded.ttl == 3U);
    test_deliver_last(&relay_rt, &leader);

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->position_valid != 0U);
    assert(record->latitude_e6 == 39908456);
    assert(record->longitude_e6 == 116397128);
}

static void test_lost_relay_dirty_state_cannot_remain_relay_parent(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 70U};
    test_runtime_t child_rt = {.name = "child", .now_s = 70U};
    sle_team_node_t leader;
    sle_team_node_t child;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&child, &child_rt, 5U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 3U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 4U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 69U;

    leader.members[1].member_id = 4U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].relay_recovery_candidate = 1U;
    leader.members[1].last_seen_s = 70U;

    leader.members[2].member_id = 5U;
    leader.members[2].online = 1U;
    leader.members[2].parent_id = 2U;
    leader.members[2].next_hop_id = 2U;
    leader.members[2].last_seen_s = 69U;

    assert(sle_team_node_member_offline(&leader, 2U) == SLE_TEAM_OK);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->relay_allowed == 0U);
    assert(record->parent_id == 0U);
    assert(leader.relay_recovery_pending != 0U);
    assert(leader.relay_recovery_lost_relay_id == 2U);
    assert(leader.relay_recovery_selected_id == 4U);

    assert(sle_team_node_send_hello(&child, child.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&child_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_heartbeat(&child, child.cfg.leader_id, 90U, -42, 1U) == SLE_TEAM_OK);
    test_deliver_last(&child_rt, &leader);
    record = sle_team_node_find_member(&leader, 5U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 4U);
    assert(record->next_hop_id == 4U);
    assert(record->relay_allowed == 0U);
}

static void test_lost_relay_id_cannot_reclaim_relay_grant(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 71U};
    sle_team_node_t leader;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.cfg.max_downstream = 3U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].last_seen_s = 70U;
    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].last_seen_s = 71U;
    leader.members[1].last_rssi_dbm = -46;
    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 2U;

    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_ERR_UNSUPPORTED);
    assert(sle_team_node_grant_relay(&leader, 3U) == SLE_TEAM_OK);

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->relay_allowed == 0U);
    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->relay_allowed != 0U);
    assert(record->relay_tier != 0U);
}

static void test_rejoin_clears_stale_relay_policy(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 30U};
    test_runtime_t member_rt = {.name = "member", .now_s = 30U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 3U, SLE_TEAM_ROLE_MEMBER);

    leader.members[0].member_id = 3U;
    leader.members[0].online = 0U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].child_count = 2U;
    leader.members[0].last_seen_s = 1U;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);

    record = sle_team_node_find_member(&leader, 3U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 1U);
    assert(record->next_hop_id == 1U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_tier == 0U);
    assert(record->max_downstream == 0U);
    assert(record->child_count == 0U);
}

static void test_relay_selection_prefers_capacity_then_rssi_then_child_count(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 40U};
    test_runtime_t member_rt = {.name = "member", .now_s = 40U};
    sle_team_node_t leader;
    sle_team_node_t member;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 9U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 3U;
    leader.members[0].child_count = 2U;
    leader.members[0].last_seen_s = 39U;
    leader.members[0].last_rssi_dbm = -20;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 4U;
    leader.members[1].child_count = 2U;
    leader.members[1].last_seen_s = 39U;
    leader.members[1].last_rssi_dbm = -80;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 9U);
    assert(record != NULL);
    assert(record->parent_id == 3U);

    leader.members[0].max_downstream = 4U;
    leader.members[0].last_rssi_dbm = -10;
    leader.members[1].max_downstream = 4U;
    leader.members[1].last_rssi_dbm = -40;
    leader.members[2].member_id = 9U;
    leader.members[2].online = 0U;
    leader.members[2].policy_pending = 0U;
    leader.members[2].parent_id = 0U;
    leader.members[2].next_hop_id = 0U;
    leader.members[2].relay_allowed = 0U;
    leader.members[2].child_count = 0U;
    leader.members[2].last_seen_s = 0U;

    assert(sle_team_node_send_hello(&member, member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 9U);
    assert(record != NULL);
    assert(record->parent_id == 2U);
}

static void test_rssi_optimizer_swaps_only_childless_weak_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 100U};
    sle_team_node_t leader;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].child_count = 0U;
    leader.members[0].last_seen_s = 100U;
    leader.members[0].last_rssi_dbm = -82;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].last_seen_s = 100U;
    leader.members[1].last_rssi_dbm = -60;

    assert(sle_team_relay_optimizer_run(&leader, 100U) == 1);
    assert(leader.members[0].relay_allowed == 0U);
    assert(leader.members[0].relay_tier == 0U);
    assert(leader.members[0].max_downstream == 0U);
    assert(leader.members[1].relay_allowed != 0U);
    assert(leader.members[1].relay_tier == 1U);
    assert(leader.members[1].max_downstream == 7U);
}

static void test_rssi_optimizer_keeps_child_bearing_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 100U};
    sle_team_node_t leader;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].child_count = 1U;
    leader.members[0].last_seen_s = 100U;
    leader.members[0].last_rssi_dbm = -90;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].last_seen_s = 100U;
    leader.members[1].last_rssi_dbm = -50;

    assert(sle_team_relay_optimizer_run(&leader, 100U) == SLE_TEAM_OK);
    assert(leader.members[0].relay_allowed != 0U);
    assert(leader.members[1].relay_allowed == 0U);
}

static void test_rssi_optimizer_skips_multi_relay_topology(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 100U};
    sle_team_node_t leader;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].child_count = 0U;
    leader.members[0].last_seen_s = 100U;
    leader.members[0].last_rssi_dbm = -86;
    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].child_count = 4U;
    leader.members[1].last_seen_s = 100U;
    leader.members[1].last_rssi_dbm = -70;
    leader.members[2].member_id = 4U;
    leader.members[2].online = 1U;
    leader.members[2].parent_id = 1U;
    leader.members[2].next_hop_id = 1U;
    leader.members[2].last_seen_s = 100U;
    leader.members[2].last_rssi_dbm = -50;

    assert(sle_team_relay_optimizer_run(&leader, 100U) == SLE_TEAM_OK);
    assert(leader.members[0].relay_allowed != 0U);
    assert(leader.members[1].relay_allowed != 0U);
    assert(leader.members[2].relay_allowed == 0U);
}

static void test_rssi_optimizer_skips_recovery_and_pending_old_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 100U};
    sle_team_node_t leader;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);

    leader.members[0].member_id = 2U;
    leader.members[0].online = 1U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 100U;
    leader.members[0].last_rssi_dbm = -88;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].last_seen_s = 100U;
    leader.members[1].last_rssi_dbm = -50;

    leader.relay_recovery_pending = 1U;
    leader.relay_recovery_lost_relay_id = 2U;
    assert(sle_team_relay_optimizer_run(&leader, 100U) == SLE_TEAM_OK);
    assert(leader.members[0].relay_allowed != 0U);
    assert(leader.members[1].relay_allowed == 0U);

    leader.relay_recovery_pending = 0U;
    leader.members[1].relay_recovery_candidate = 1U;
    assert(sle_team_relay_optimizer_run(&leader, 100U) == SLE_TEAM_OK);
    assert(leader.members[0].relay_allowed != 0U);
    assert(leader.members[1].relay_allowed == 0U);
}

static void test_returning_old_relay_after_rssi_swap_joins_current_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 120U};
    test_runtime_t old_relay_rt = {.name = "old-relay", .now_s = 120U};
    sle_team_node_t leader;
    sle_team_node_t old_relay;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_relay, &old_relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    leader.members[0].member_id = 2U;
    leader.members[0].online = 0U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 1U;
    leader.members[0].relay_tier = 1U;
    leader.members[0].max_downstream = 7U;
    leader.members[0].last_seen_s = 80U;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].last_seen_s = 120U;
    leader.members[1].last_rssi_dbm = -55;

    assert(sle_team_node_send_hello(&old_relay, old_relay.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);
    assert(record->relay_tier == 0U);
    assert(record->max_downstream == 0U);

    assert(sle_team_node_send_heartbeat(&old_relay, old_relay.cfg.leader_id, 90U, -50, 1U) == SLE_TEAM_OK);
    test_deliver_last(&old_relay_rt, &leader);

    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);
}

static void test_returning_old_member_after_rssi_swap_joins_current_relay(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 130U};
    test_runtime_t old_member_rt = {.name = "old-member", .now_s = 130U};
    sle_team_node_t leader;
    sle_team_node_t old_member;
    const sle_team_member_record_t *record;
    const sle_team_member_record_t *relay;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&old_member, &old_member_rt, 4U, SLE_TEAM_ROLE_MEMBER);
    leader.cfg.max_downstream = 1U;

    leader.members[0].member_id = 4U;
    leader.members[0].online = 0U;
    leader.members[0].parent_id = 1U;
    leader.members[0].next_hop_id = 1U;
    leader.members[0].relay_allowed = 0U;
    leader.members[0].last_seen_s = 90U;

    leader.members[1].member_id = 3U;
    leader.members[1].online = 1U;
    leader.members[1].parent_id = 1U;
    leader.members[1].next_hop_id = 1U;
    leader.members[1].relay_allowed = 1U;
    leader.members[1].relay_tier = 1U;
    leader.members[1].max_downstream = 7U;
    leader.members[1].last_seen_s = 130U;
    leader.members[1].last_rssi_dbm = -54;

    assert(sle_team_node_send_hello(&old_member, old_member.cfg.leader_id) == SLE_TEAM_OK);
    test_deliver_last(&old_member_rt, &leader);

    record = sle_team_node_find_member(&leader, 4U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_heartbeat(&old_member, old_member.cfg.leader_id, 90U, -52, 1U) == SLE_TEAM_OK);
    test_deliver_last(&old_member_rt, &leader);

    record = sle_team_node_find_member(&leader, 4U);
    assert(record != NULL);
    assert(record->online != 0U);
    assert(record->policy_pending == 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);
    relay = sle_team_node_find_member(&leader, 3U);
    assert(relay != NULL);
    assert(relay->relay_allowed != 0U);
    assert(relay->child_count == 1U);
}

static void test_rssi_swap_then_higher_term_new_leader_returning_member_joins_current_relay(void)
{
    test_runtime_t base_leader_rt = {.name = "base-leader", .now_s = 140U};
    test_runtime_t new_leader_rt = {.name = "new-leader", .now_s = 141U};
    test_runtime_t returning_rt = {.name = "returning", .now_s = 141U};
    test_runtime_t old_leader_rt = {.name = "old-leader", .now_s = 141U};
    sle_team_node_t base_leader;
    sle_team_node_t new_leader;
    sle_team_node_t returning;
    sle_team_node_t old_leader;
    const sle_team_member_record_t *record;
    const sle_team_member_record_t *relay;

    test_init_node(&base_leader, &base_leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    base_leader.members[0].member_id = 2U;
    base_leader.members[0].online = 1U;
    base_leader.members[0].parent_id = 1U;
    base_leader.members[0].next_hop_id = 1U;
    base_leader.members[0].relay_allowed = 1U;
    base_leader.members[0].relay_tier = 1U;
    base_leader.members[0].max_downstream = 7U;
    base_leader.members[0].child_count = 0U;
    base_leader.members[0].last_seen_s = 140U;
    base_leader.members[0].last_rssi_dbm = -84;
    base_leader.members[1].member_id = 3U;
    base_leader.members[1].online = 1U;
    base_leader.members[1].parent_id = 1U;
    base_leader.members[1].next_hop_id = 1U;
    base_leader.members[1].last_seen_s = 140U;
    base_leader.members[1].last_rssi_dbm = -52;
    assert(sle_team_relay_optimizer_run(&base_leader, 140U) == 1);
    assert(base_leader.members[0].relay_allowed == 0U);
    assert(base_leader.members[1].relay_allowed != 0U);

    test_init_node(&new_leader, &new_leader_rt, 4U, SLE_TEAM_ROLE_LEADER);
    new_leader.cfg.leader_term = 2U;
    new_leader.cfg.max_downstream = 1U;
    new_leader.members[0].member_id = 3U;
    new_leader.members[0].online = 1U;
    new_leader.members[0].parent_id = 4U;
    new_leader.members[0].next_hop_id = 4U;
    new_leader.members[0].relay_allowed = 1U;
    new_leader.members[0].relay_tier = 1U;
    new_leader.members[0].max_downstream = 7U;
    new_leader.members[0].last_seen_s = 141U;
    new_leader.members[0].last_rssi_dbm = -52;

    test_init_node(&returning, &returning_rt, 5U, SLE_TEAM_ROLE_MEMBER);
    returning.cfg.leader_id = 1U;
    returning.cfg.leader_term = 1U;
    returning.joined = 0U;
    returning.state = SLE_TEAM_NET_WAIT_POLICY;
    returning.cfg.relay_allowed = 1U;
    returning.cfg.relay_enabled = 1U;
    returning.cfg.relay_tier = 1U;
    returning.cfg.max_downstream = 7U;

    assert(sle_team_node_member_select_leader_term(&returning, 1U, 4U, 0x11U, 2U) == SLE_TEAM_OK);
    test_deliver_last(&returning_rt, &new_leader);
    record = sle_team_node_find_member(&new_leader, 5U);
    assert(record != NULL);
    assert(record->policy_pending != 0U);
    assert(record->parent_id == 3U);
    assert(record->next_hop_id == 3U);
    assert(record->relay_allowed == 0U);

    assert(sle_team_node_send_route_update(&new_leader, 5U, 3U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 3U) == SLE_TEAM_OK);
    test_deliver_last(&new_leader_rt, &returning);
    assert(returning.cfg.leader_id == 4U);
    assert(returning.cfg.leader_term == 2U);
    assert(returning.joined != 0U);
    assert(returning.upstream_parent_id == 3U);
    assert(returning.cfg.relay_allowed == 0U);
    relay = sle_team_node_find_member(&new_leader, 3U);
    assert(relay != NULL);
    assert(relay->relay_allowed != 0U);

    test_init_node(&old_leader, &old_leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    old_leader.cfg.leader_term = 1U;
    old_leader.members[0].member_id = 5U;
    old_leader.members[0].online = 1U;
    old_leader.members[0].parent_id = 1U;
    old_leader.members[0].next_hop_id = 1U;
    old_leader.members[0].relay_allowed = 1U;
    old_leader.members[0].relay_tier = 1U;
    old_leader.members[0].max_downstream = 7U;
    assert(sle_team_node_send_route_update(&old_leader, 5U, 1U,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(&returning, old_leader_rt.last_tx, old_leader_rt.last_tx_len) ==
        SLE_TEAM_ERR_UNSUPPORTED);
    assert(returning.cfg.leader_id == 4U);
    assert(returning.cfg.leader_term == 2U);
    assert(returning.upstream_parent_id == 3U);
}

static void test_nmea_rmc_gga_updates_position(void)
{
    sle_team_nmea_state_t nmea;
    sle_team_pos_body_t pos;

    sle_team_nmea_init(&nmea);
    assert(sle_team_nmea_parse_line(&nmea,
        "$GPRMC,123519,A,4807.038,N,01131.000,E,022.4,084.4,230394,003.1,W*6A", &pos) == SLE_TEAM_OK);
    assert(pos.fix_status == 1U);
    assert(pos.latitude_e6 == 48117300);
    assert(pos.longitude_e6 == 11516666);
    assert(pos.speed_cms == 1152U);
    assert(pos.heading_deg == 84U);
    assert(sle_team_nmea_parse_line(&nmea,
        "$GPGGA,123519,4807.038,N,01131.000,E,1,08,0.9,545.4,M,46.9,M,,*47", &pos) == SLE_TEAM_OK);
    assert(pos.fix_status == 1U);
    assert(pos.sat_count == 8U);
    assert(pos.latitude_e6 == 48117300);
    assert(pos.longitude_e6 == 11516666);
}

static void test_position_report_survives_member_offline(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 10U};
    test_runtime_t member_rt = {.name = "member", .now_s = 10U};
    sle_team_node_t leader;
    sle_team_node_t member;
    sle_team_pos_body_t pos;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    test_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    test_join_member(&leader, &leader_rt, &member, &member_rt);
    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -42, 1U) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    (void)memset(&pos, 0, sizeof(pos));
    pos.latitude_e6 = 39908456;
    pos.longitude_e6 = 116397128;
    pos.speed_cms = 120U;
    pos.heading_deg = 90U;
    pos.battery_percent = 87U;
    pos.fix_status = 1U;
    pos.sat_count = 9U;
    assert(sle_team_node_send_position(&member, member.cfg.leader_id, &pos) == SLE_TEAM_OK);
    test_deliver_last(&member_rt, &leader);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->position_valid != 0U);
    assert(record->latitude_e6 == 39908456);
    assert(sle_team_node_member_offline(&leader, 2U) == SLE_TEAM_OK);
    record = sle_team_node_find_member(&leader, 2U);
    assert(record != NULL);
    assert(record->online == 0U);
    assert(record->position_valid != 0U);
    assert(record->latitude_e6 == 39908456);
    assert(record->longitude_e6 == 116397128);
}

static void test_leader_records_local_phone_position_without_online_member(void)
{
    test_runtime_t leader_rt = {.name = "leader", .now_s = 80U};
    sle_team_node_t leader;
    sle_team_pos_body_t pos;
    const sle_team_member_record_t *record;

    test_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    (void)memset(&pos, 0, sizeof(pos));
    pos.latitude_e6 = 31234567;
    pos.longitude_e6 = 121123456;
    pos.speed_cms = 12U;
    pos.heading_deg = 270U;
    pos.battery_percent = 100U;
    pos.fix_status = 1U;
    pos.sat_count = 0U;
    assert(sle_team_node_record_local_position(&leader, &pos) == SLE_TEAM_OK);
    record = sle_team_node_find_member(&leader, 1U);
    assert(record != NULL);
    assert(record->role == SLE_TEAM_ROLE_LEADER);
    assert(record->position_valid != 0U);
    assert(record->latitude_e6 == 31234567);
    assert(record->longitude_e6 == 121123456);
    assert(record->sat_count == 0U);
    assert(record->parent_id == 0U);
    assert(record->next_hop_id == 0U);
    assert(record->online == 0U);
    assert(record->relay_allowed == 0U);
}

int main(void)
{
    test_direct_join_and_link_lost_hello_loop();
    test_member_leave_sends_leave_alert_before_idle();
    test_leader_rejects_mismatched_firmware_hello();
    test_leader_rejects_missing_firmware_hello();
    test_scan_response_fw_compat_and_route_id_from_raw_bytes();
    test_app_packet_carries_leader_term();
    test_member_select_new_leader_clears_stale_parent_and_relay_state();
    test_member_select_new_leader_can_rejoin_fresh_policy();
    test_member_on_new_leader_ignores_stale_old_leader_policy();
    test_member_rejects_lower_term_old_leader_packets();
    test_relay_rejects_forwarding_lower_term_old_leader_downlink();
    test_member_accepts_higher_term_new_leader_route();
    test_member_rejects_non_higher_term_other_leader_route();
    test_relay_forwards_stale_child_hello_to_current_leader();
    test_old_member_rejoins_higher_term_leader_through_current_relay();
    test_member_select_new_leader_joins_current_relay_when_direct_full();
    test_direct_member_offline_releases_slot_for_rejoin();
    test_direct_capacity_and_relay_overflow_policy();
    test_direct_capacity_auto_grants_first_relay_for_overflow();
    test_forwarded_initial_hello_uses_ingress_relay_parent();
    test_overflow_promotes_second_relay_when_existing_relay_is_full();
    test_hello_waits_for_heartbeat_and_reserves_direct_slot();
    test_repeated_hello_keeps_stable_member_parent();
    test_ack_confirms_only_matching_pending_policy();
    test_route_update_ack_confirms_pending_policy();
    test_pending_policy_send_failure_keeps_previous_ack_target();
    test_pending_policy_timeout_releases_direct_slot();
    test_pending_relay_child_reserves_relay_capacity();
    test_online_direct_member_can_be_granted_relay_before_overflow();
    test_repeated_hello_reassigns_pending_member_to_relay();
    test_repeated_hello_reassigns_pending_member_to_direct_but_keeps_relay_delivery_hop();
    test_connected_route_update_rejoins_member_without_hello_ack();
    test_pairing_approve_respects_pending_confirmation();
    test_relay_forward_decrements_ttl();
    test_relay_leader_downlink_refreshes_parent_liveness();
    test_relay_timeout_marks_children_offline_without_callback();
    test_relay_loss_selects_replacement_child_and_old_relay_rejoins_as_child();
    test_fast_old_relay_hello_after_recovery_rejoins_as_child();
    test_relay_recovery_keeps_remaining_candidates_on_replacement_relay();
    test_selected_recovery_relay_preserves_live_next_hop();
    test_relay_recovery_heartbeat_reassigns_stale_records();
    test_pending_relay_child_position_confirms_through_current_relay();
    test_lost_relay_dirty_state_cannot_remain_relay_parent();
    test_lost_relay_id_cannot_reclaim_relay_grant();
    test_rejoin_clears_stale_relay_policy();
    test_relay_selection_prefers_capacity_then_rssi_then_child_count();
    test_rssi_optimizer_swaps_only_childless_weak_relay();
    test_rssi_optimizer_keeps_child_bearing_relay();
    test_rssi_optimizer_skips_multi_relay_topology();
    test_rssi_optimizer_skips_recovery_and_pending_old_relay();
    test_returning_old_relay_after_rssi_swap_joins_current_relay();
    test_returning_old_member_after_rssi_swap_joins_current_relay();
    test_rssi_swap_then_higher_term_new_leader_returning_member_joins_current_relay();
    test_nmea_rmc_gga_updates_position();
    test_position_report_survives_member_offline();
    test_leader_records_local_phone_position_without_online_member();
    printf("[team-node-regression] minimal pass\n");
    return 0;
}
