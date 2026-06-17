#include "sle_team_node.h"
#include "sle_team_web_api.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    uint32_t now_s;
    uint8_t last_tx[256];
    uint16_t last_tx_len;
    uint8_t joined_count;
    uint8_t position_count;
    uint8_t relay_offline_count;
    const char *name;
} demo_runtime_t;

static int demo_send(void *user_ctx, sle_team_send_kind_t kind, uint8_t dst_id, const uint8_t *buf, uint16_t len)
{
    demo_runtime_t *rt = (demo_runtime_t *)user_ctx;

    (void)kind;
    (void)dst_id;
    if (rt == NULL || buf == NULL || len > sizeof(rt->last_tx)) {
        return -1;
    }
    (void)memcpy(rt->last_tx, buf, len);
    rt->last_tx_len = len;
    return SLE_TEAM_OK;
}

static uint32_t demo_now(void *user_ctx)
{
    const demo_runtime_t *rt = (const demo_runtime_t *)user_ctx;
    return rt == NULL ? 0U : rt->now_s;
}

static void demo_joined(void *user_ctx, uint8_t member_id)
{
    demo_runtime_t *rt = (demo_runtime_t *)user_ctx;

    (void)member_id;
    if (rt != NULL) {
        rt->joined_count++;
    }
}

static void demo_position(void *user_ctx, uint8_t member_id, const sle_team_pos_body_t *pos)
{
    demo_runtime_t *rt = (demo_runtime_t *)user_ctx;

    (void)member_id;
    (void)pos;
    if (rt != NULL) {
        rt->position_count++;
    }
}

static void demo_relay_offline(void *user_ctx, uint8_t member_id)
{
    demo_runtime_t *rt = (demo_runtime_t *)user_ctx;

    (void)member_id;
    if (rt != NULL) {
        rt->relay_offline_count++;
    }
}

static void demo_init_node(sle_team_node_t *node, demo_runtime_t *rt, uint8_t self_id,
    sle_team_node_role_t role)
{
    sle_team_node_cfg_t cfg;
    sle_team_node_ops_t ops;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.team_id = 1U;
    cfg.self_id = self_id;
    cfg.leader_id = 1U;
    cfg.role = role;
    cfg.channel_hash = 0x11U;
    cfg.heartbeat_interval_s = 1U;
    cfg.heartbeat_timeout_s = 3U;
    cfg.report_interval_s = 5U;
    cfg.default_ttl = 4U;

    (void)memset(&ops, 0, sizeof(ops));
    ops.send = demo_send;
    ops.now_s = demo_now;
    ops.on_joined = demo_joined;
    ops.on_position = demo_position;
    ops.on_relay_offline = demo_relay_offline;
    ops.user_ctx = rt;

    assert(sle_team_node_init(node, &cfg, &ops) == SLE_TEAM_OK);
}

static void demo_deliver_last(demo_runtime_t *from, sle_team_node_t *to)
{
    if (from->last_tx_len == 0U) {
        return;
    }
    assert(sle_team_node_on_packet(to, from->last_tx, from->last_tx_len) == SLE_TEAM_OK);
    from->last_tx_len = 0U;
}

static void demo_join_member(sle_team_node_t *leader, demo_runtime_t *leader_rt,
    sle_team_node_t *member, demo_runtime_t *member_rt)
{
    assert(sle_team_node_send_hello(member, member->cfg.leader_id) == SLE_TEAM_OK);
    demo_deliver_last(member_rt, leader);
    demo_deliver_last(leader_rt, member);
    assert(member->joined != 0U);
}

static void demo_confirm_member_by_heartbeat(sle_team_node_t *leader, uint8_t member_id, uint32_t now_s)
{
    demo_runtime_t member_rt = {.name = "confirm", .now_s = now_s};
    sle_team_node_t member;

    demo_init_node(&member, &member_rt, member_id, SLE_TEAM_ROLE_MEMBER);
    assert(sle_team_node_send_heartbeat(&member, member.cfg.leader_id, 90U, -40, 1U) == SLE_TEAM_OK);
    assert(sle_team_node_on_packet(leader, member_rt.last_tx, member_rt.last_tx_len) == SLE_TEAM_OK);
}

static void demo_direct_join_and_status_json(void)
{
    demo_runtime_t leader_rt = {.name = "leader", .now_s = 10U};
    demo_runtime_t member_rt = {.name = "member", .now_s = 10U};
    sle_team_node_t leader;
    sle_team_node_t member;
    char json[512];

    demo_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    demo_init_node(&member, &member_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    demo_join_member(&leader, &leader_rt, &member, &member_rt);

    assert(member.upstream_parent_id == 1U);
    assert(member.upstream_parent_state == SLE_TEAM_PARENT_CONNECTED);
    assert(sle_team_node_find_member(&leader, 2U) != NULL);
    assert(sle_team_web_write_status_json(&member, 10U, "sim", json, sizeof(json)) >= 0);
    assert(strstr(json, "\"upstreamParentState\":\"connected\"") != NULL);
}

static void demo_relay_overflow_and_forward(void)
{
    demo_runtime_t leader_rt = {.name = "leader", .now_s = 20U};
    demo_runtime_t relay_rt = {.name = "relay", .now_s = 20U};
    demo_runtime_t child_rt = {.name = "child", .now_s = 20U};
    sle_team_node_t leader;
    sle_team_node_t relay;
    sle_team_node_t child;
    const sle_team_member_record_t *overflow;
    const sle_team_member_record_t *relay_record;
    sle_team_pos_body_t pos;
    uint8_t id;

    demo_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    demo_init_node(&relay, &relay_rt, 2U, SLE_TEAM_ROLE_MEMBER);
    demo_init_node(&child, &child_rt, 9U, SLE_TEAM_ROLE_MEMBER);

    for (id = 2U; id <= 8U; id++) {
        assert(sle_team_node_pairing_approve(&leader, id) == SLE_TEAM_OK);
        demo_confirm_member_by_heartbeat(&leader, id, leader_rt.now_s);
    }
    assert(sle_team_node_grant_relay(&leader, 2U) == SLE_TEAM_OK);
    assert(sle_team_node_pairing_approve(&leader, 9U) == SLE_TEAM_OK);
    demo_confirm_member_by_heartbeat(&leader, 9U, leader_rt.now_s);

    overflow = sle_team_node_find_member(&leader, 9U);
    assert(overflow != NULL);
    assert(overflow->parent_id != 1U);
    relay_record = sle_team_node_find_member(&leader, overflow->parent_id);
    assert(relay_record != NULL);
    assert(relay_record->relay_allowed != 0U);
    assert(relay_record->child_count == 1U);

    assert(sle_team_node_send_config(&leader, overflow->parent_id) == SLE_TEAM_OK);
    demo_deliver_last(&leader_rt, &relay);
    assert(relay.cfg.relay_allowed != 0U);
    relay.joined = 1U;
    relay.cfg.relay_enabled = 1U;
    relay.upstream_parent_id = 1U;
    relay.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;

    child.joined = 1U;
    child.upstream_parent_id = overflow->parent_id;
    child.upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
    (void)memset(&pos, 0, sizeof(pos));
    pos.battery_percent = 90U;
    assert(sle_team_node_send_position(&child, 1U, &pos) == SLE_TEAM_OK);
    demo_deliver_last(&child_rt, &relay);
    assert(relay_rt.last_tx_len != 0U);
    demo_deliver_last(&relay_rt, &leader);
    assert(leader_rt.position_count == 1U);
}

static void demo_timeout_marks_member_offline(void)
{
    demo_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    sle_team_node_t leader;

    demo_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    assert(sle_team_node_pairing_approve_with_relay(&leader, 2U, 1U) == SLE_TEAM_OK);
    demo_confirm_member_by_heartbeat(&leader, 2U, leader_rt.now_s);
    assert(sle_team_node_find_member(&leader, 2U) != NULL);
    leader.members[0].last_seen_s = 1U;
    leader_rt.now_s = 6U;
    sle_team_node_tick(&leader);
    assert(sle_team_node_find_member(&leader, 2U)->online == 0U);
    assert(leader_rt.relay_offline_count == 1U);
}

static void demo_nodes_json_keeps_offline_last_position(void)
{
    demo_runtime_t leader_rt = {.name = "leader", .now_s = 1U};
    sle_team_node_t leader;
    char json[768];

    demo_init_node(&leader, &leader_rt, 1U, SLE_TEAM_ROLE_LEADER);
    leader.members[0].member_id = 2U;
    leader.members[0].role = SLE_TEAM_ROLE_MEMBER;
    leader.members[0].online = 0U;
    leader.members[0].policy_pending = 0U;
    leader.members[0].battery_percent = 77U;
    leader.members[0].fix_status = 1U;
    leader.members[0].position_valid = 1U;
    leader.members[0].latitude_e6 = 39908456;
    leader.members[0].longitude_e6 = 116397128;
    leader.members[0].speed_cms = 120U;
    leader.members[0].heading_deg = 90U;
    leader.members[0].sat_count = 9U;
    leader.members[0].last_seen_s = 55U;
    assert(sle_team_web_write_nodes_json(&leader, json, sizeof(json)) >= 0);
    assert(strstr(json, "\"id\":2") != NULL);
    assert(strstr(json, "\"online\":false") != NULL);
    assert(strstr(json, "\"positionValid\":true") != NULL);
    assert(strstr(json, "\"latitudeE6\":39908456") != NULL);
    assert(strstr(json, "\"longitudeE6\":116397128") != NULL);
}

#ifdef SLE_TEAM_NETWORK_TEST
int main(void)
{
    demo_direct_join_and_status_json();
    demo_relay_overflow_and_forward();
    demo_timeout_marks_member_offline();
    demo_nodes_json_keeps_offline_last_position();
    printf("[team-network-demo] minimal pass\n");
    return 0;
}
#endif
