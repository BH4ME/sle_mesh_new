#include "sle_team_node.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define SUITE_AUTO_RELAY_MAX 3U
#define SUITE_REBALANCE_INTERVAL_S 8U
#define SUITE_RELAY_REVOKE_STALE_FACTOR 2U
#define SUITE_RELAY_CANDIDATE_MIN_RSSI (-92)
#define SUITE_DEFAULT_TIMEOUT_S 10U
#define SUITE_MAX_LOGICAL_MEMBERS 30U

typedef struct {
    uint8_t member_id;
    uint8_t online;
    uint8_t relay_allowed;
    int8_t last_rssi_dbm;
    uint32_t last_seen_s;
    uint8_t parent_id;
} suite_member_t;

typedef struct {
    suite_member_t members[SLE_TEAM_MAX_MEMBERS];
    uint8_t self_id;
    uint8_t leader_id;
    uint8_t pairing_enabled;
    uint8_t role_configured;
    uint8_t sle_started;
    uint16_t heartbeat_timeout_s;
    uint32_t relay_rebalance_last_s;
    uint8_t relay_online_count;
    uint8_t relay_target_count;
    uint32_t route_reparent_total;
    uint8_t inject_send_fail_member_id;
    uint8_t inject_send_fail_once;
    uint8_t pairing_hidden_relay_only;
} suite_cluster_t;

static uint32_t suite_elapsed_s(uint32_t now_s, uint32_t since_s)
{
    return (uint32_t)(now_s - since_s);
}

static uint8_t suite_interval_not_reached(uint32_t now_s, uint32_t last_s, uint32_t interval_s)
{
    if (last_s == 0U || interval_s == 0U) {
        return 0U;
    }
    return (uint8_t)(suite_elapsed_s(now_s, last_s) < interval_s);
}

static uint8_t suite_elapsed_exceeds(uint32_t now_s, uint32_t since_s, uint32_t limit_s)
{
    if (since_s == 0U || limit_s == 0U) {
        return 0U;
    }
    return (uint8_t)(suite_elapsed_s(now_s, since_s) > limit_s);
}

static uint8_t suite_relay_target_for_online(uint8_t online_count)
{
    if (online_count == 0U || online_count <= SLE_TEAM_MAX_DIRECT_CONNECTIONS) {
        return 0U;
    }
    if (online_count <= (uint8_t)(SLE_TEAM_MAX_DIRECT_CONNECTIONS * 2U)) {
        return 2U;
    }
    return SUITE_AUTO_RELAY_MAX;
}

static suite_member_t *suite_find_member(suite_cluster_t *cluster, uint8_t member_id)
{
    uint8_t i;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (cluster->members[i].member_id == member_id) {
            return &cluster->members[i];
        }
    }
    return NULL;
}

static const suite_member_t *suite_find_member_const(const suite_cluster_t *cluster, uint8_t member_id)
{
    uint8_t i;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (cluster->members[i].member_id == member_id) {
            return &cluster->members[i];
        }
    }
    return NULL;
}

static void suite_set_member_rssi(suite_cluster_t *cluster, uint8_t member_id, int8_t rssi_dbm)
{
    suite_member_t *member = suite_find_member(cluster, member_id);

    assert(member != NULL);
    member->last_rssi_dbm = rssi_dbm;
}

static void suite_route_clear_by_next_hop(suite_cluster_t *cluster, uint8_t next_hop_id)
{
    uint8_t i;

    if (next_hop_id == 0U || next_hop_id == SLE_TEAM_BROADCAST_ID) {
        return;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
            continue;
        }
        if (member->parent_id == next_hop_id) {
            member->parent_id = 0U;
        }
    }
}

static uint8_t suite_set_member_relay_allowed(suite_cluster_t *cluster, suite_member_t *member, uint8_t relay_allowed)
{
    uint8_t old_allowed;

    if (member == NULL || member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }

    old_allowed = member->relay_allowed;
    if (old_allowed == (relay_allowed != 0U ? 1U : 0U)) {
        return 1U;
    }

    member->relay_allowed = relay_allowed != 0U ? 1U : 0U;
    if (cluster->inject_send_fail_once != 0U && cluster->inject_send_fail_member_id == member->member_id) {
        cluster->inject_send_fail_once = 0U;
        member->relay_allowed = old_allowed;
        return 0U;
    }

    if (member->relay_allowed == 0U) {
        suite_route_clear_by_next_hop(cluster, member->member_id);
    }
    return 1U;
}

static uint8_t suite_relay_is_candidate(const suite_cluster_t *cluster, const suite_member_t *member,
    uint32_t now_s, uint16_t timeout_s)
{
    if (member == NULL || member->online == 0U) {
        return 0U;
    }
    if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID ||
        member->member_id == cluster->self_id || member->member_id == cluster->leader_id) {
        return 0U;
    }
    if (member->relay_allowed != 0U) {
        return 0U;
    }
    if (suite_elapsed_exceeds(now_s, member->last_seen_s, (uint32_t)timeout_s) != 0U) {
        return 0U;
    }
    if (member->last_rssi_dbm != SLE_TEAM_RSSI_UNKNOWN && member->last_rssi_dbm < SUITE_RELAY_CANDIDATE_MIN_RSSI) {
        return 0U;
    }
    return 1U;
}

static suite_member_t *suite_pick_best_candidate(suite_cluster_t *cluster, uint32_t now_s, uint16_t timeout_s)
{
    uint8_t i;
    suite_member_t *best = NULL;
    int8_t best_rssi = SLE_TEAM_RSSI_UNKNOWN;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];
        int8_t member_rssi;

        if (suite_relay_is_candidate(cluster, member, now_s, timeout_s) == 0U) {
            continue;
        }
        /* Keep simulation aligned with production: unknown RSSI is weakest. */
        member_rssi = member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN ? (int8_t)(-128) : member->last_rssi_dbm;
        if (best == NULL || member_rssi > best_rssi) {
            best = member;
            best_rssi = member_rssi;
        }
    }
    return best;
}

static suite_member_t *suite_pick_worst_active_relay(suite_cluster_t *cluster, uint32_t now_s, uint16_t timeout_s)
{
    uint8_t i;
    suite_member_t *worst = NULL;
    int8_t worst_rssi = (int8_t)(SLE_TEAM_RSSI_UNKNOWN + 1);
    uint32_t worst_age_s = 0U;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];
        int8_t member_rssi;
        uint32_t member_age_s;

        if (member->online == 0U || member->relay_allowed == 0U ||
            member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID ||
            member->member_id == cluster->self_id || member->member_id == cluster->leader_id) {
            continue;
        }
        if (suite_elapsed_exceeds(now_s, member->last_seen_s,
                (uint32_t)timeout_s * SUITE_RELAY_REVOKE_STALE_FACTOR) != 0U) {
            continue;
        }
        /* Unknown RSSI must be treated as weakest so known links are demoted first. */
        member_rssi = member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN ? (int8_t)(-128) : member->last_rssi_dbm;
        member_age_s = suite_elapsed_s(now_s, member->last_seen_s);
        if (worst == NULL || member_rssi < worst_rssi ||
            (member_rssi == worst_rssi && member_age_s > worst_age_s) ||
            (member_rssi == worst_rssi && member_age_s == worst_age_s && member->member_id > worst->member_id)) {
            worst = member;
            worst_rssi = member_rssi;
            worst_age_s = member_age_s;
        }
    }
    return worst;
}

static void suite_rebalance_relays(suite_cluster_t *cluster, uint32_t now_s)
{
    uint16_t timeout_s;
    uint8_t i;
    uint8_t online_count = 0U;
    uint8_t relay_count = 0U;
    uint8_t relay_target;

    if (cluster->role_configured == 0U || cluster->sle_started == 0U || cluster->pairing_enabled != 0U) {
        return;
    }
    if (suite_interval_not_reached(now_s, cluster->relay_rebalance_last_s, SUITE_REBALANCE_INTERVAL_S) != 0U) {
        return;
    }
    cluster->relay_rebalance_last_s = now_s;
    timeout_s = cluster->heartbeat_timeout_s != 0U ? cluster->heartbeat_timeout_s : SUITE_DEFAULT_TIMEOUT_S;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID || member->relay_allowed == 0U) {
            continue;
        }
        if (member->online == 0U) {
            member->relay_allowed = 0U;
            suite_route_clear_by_next_hop(cluster, member->member_id);
            continue;
        }
        if (suite_elapsed_exceeds(now_s, member->last_seen_s,
                (uint32_t)timeout_s * SUITE_RELAY_REVOKE_STALE_FACTOR) != 0U) {
            (void)suite_set_member_relay_allowed(cluster, member, 0U);
        }
    }

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];

        if (member->online == 0U || member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
            continue;
        }
        online_count++;
        if (member->relay_allowed != 0U) {
            relay_count++;
        }
    }

    relay_target = suite_relay_target_for_online(online_count);
    if (relay_target > SUITE_AUTO_RELAY_MAX) {
        relay_target = SUITE_AUTO_RELAY_MAX;
    }

    while (relay_count > relay_target) {
        suite_member_t *victim = suite_pick_worst_active_relay(cluster, now_s, timeout_s);

        if (victim == NULL) {
            break;
        }
        if (suite_set_member_relay_allowed(cluster, victim, 0U) == 0U) {
            break;
        }
        relay_count--;
    }

    while (relay_count < relay_target) {
        suite_member_t *candidate = suite_pick_best_candidate(cluster, now_s, timeout_s);

        if (candidate == NULL) {
            break;
        }
        if (suite_set_member_relay_allowed(cluster, candidate, 1U) == 0U) {
            break;
        }
        relay_count++;
    }

    cluster->relay_online_count = relay_count;
    cluster->relay_target_count = relay_target;
}

static void suite_leaf_reselect_parent(suite_cluster_t *cluster, uint8_t leaf_id, uint8_t new_parent)
{
    suite_member_t *leaf = suite_find_member(cluster, leaf_id);

    assert(leaf != NULL);
    if (leaf->parent_id != new_parent) {
        cluster->route_reparent_total++;
    }
    leaf->parent_id = new_parent;
}

static uint8_t suite_leaf_can_report(const suite_cluster_t *cluster, uint8_t leaf_id)
{
    const suite_member_t *leaf = suite_find_member_const(cluster, leaf_id);
    const suite_member_t *parent;

    if (leaf == NULL || leaf->online == 0U || leaf->parent_id == 0U || leaf->parent_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    if (leaf->parent_id == cluster->leader_id) {
        return 1U;
    }
    parent = suite_find_member_const(cluster, leaf->parent_id);
    if (parent == NULL) {
        return 0U;
    }
    return (uint8_t)(parent->online != 0U && parent->relay_allowed != 0U);
}

static uint8_t suite_leaf_can_route_msg(const suite_cluster_t *cluster, uint8_t leaf_id, uint8_t app_msg_type)
{
    const suite_member_t *leaf = suite_find_member_const(cluster, leaf_id);
    const suite_member_t *parent;

    if (leaf == NULL || leaf->online == 0U || leaf->parent_id == 0U || leaf->parent_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    if (leaf->parent_id == cluster->leader_id) {
        return 1U;
    }
    parent = suite_find_member_const(cluster, leaf->parent_id);
    if (parent == NULL || parent->online == 0U || parent->relay_allowed == 0U) {
        return 0U;
    }
    if (cluster->pairing_hidden_relay_only != 0U &&
        app_msg_type != SLE_TEAM_APP_HELLO && app_msg_type != SLE_TEAM_APP_ROUTE_UPDATE) {
        return 0U;
    }
    return 1U;
}

static uint8_t suite_count_online_relays(const suite_cluster_t *cluster)
{
    uint8_t i;
    uint8_t count = 0U;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const suite_member_t *member = &cluster->members[i];

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID || member->online == 0U) {
            continue;
        }
        if (member->relay_allowed != 0U) {
            count++;
        }
    }
    return count;
}

static void suite_set_online_count(suite_cluster_t *cluster, uint8_t online_count)
{
    uint8_t i;

    assert(online_count <= SUITE_MAX_LOGICAL_MEMBERS);
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];
        uint8_t logical_index;

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
            continue;
        }
        logical_index = (uint8_t)(member->member_id - 2U);
        member->online = logical_index < online_count ? 1U : 0U;
    }
}

static void suite_seed_cluster(suite_cluster_t *cluster)
{
    uint8_t id;

    (void)memset(cluster, 0, sizeof(*cluster));
    cluster->self_id = 1U;
    cluster->leader_id = 1U;
    cluster->role_configured = 1U;
    cluster->sle_started = 1U;
    cluster->heartbeat_timeout_s = SUITE_DEFAULT_TIMEOUT_S;
    assert(SLE_TEAM_MAX_MEMBERS >= SUITE_MAX_LOGICAL_MEMBERS);

    for (id = 2U; id <= 31U; id++) {
        suite_member_t *member = &cluster->members[id - 2U];

        member->member_id = id;
        member->online = 1U;
        member->relay_allowed = (id == 2U || id == 3U || id == 4U) ? 1U : 0U;
        member->last_seen_s = 100U;
        member->last_rssi_dbm = (int8_t)(-70 - (int8_t)(id - 2U));
        member->parent_id = 1U;
    }

    /* Keep fallback candidates deterministic for relay promotion order. */
    suite_set_member_rssi(cluster, 5U, -55);
    suite_set_member_rssi(cluster, 6U, -60);
    suite_set_member_rssi(cluster, 7U, -65);
    suite_set_member_rssi(cluster, 8U, -75);
}

static void suite_refresh_online_seen(suite_cluster_t *cluster, uint32_t now_s)
{
    uint8_t i;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        suite_member_t *member = &cluster->members[i];

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID || member->online == 0U) {
            continue;
        }
        member->last_seen_s = now_s;
    }
}

static void scenario_single_relay_drop_leaf_recover(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay3;
    suite_member_t *relay5;

    suite_seed_cluster(&cluster);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    suite_leaf_reselect_parent(&cluster, 10U, 3U);
    suite_leaf_reselect_parent(&cluster, 11U, 3U);
    assert(suite_leaf_can_report(&cluster, 10U) != 0U);
    assert(suite_leaf_can_report(&cluster, 11U) != 0U);

    relay3->online = 0U;
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);
    assert(suite_leaf_can_report(&cluster, 10U) == 0U);
    assert(suite_leaf_can_report(&cluster, 11U) == 0U);

    suite_leaf_reselect_parent(&cluster, 10U, 5U);
    suite_leaf_reselect_parent(&cluster, 11U, 5U);
    assert(suite_leaf_can_report(&cluster, 10U) != 0U);
    assert(suite_leaf_can_report(&cluster, 11U) != 0U);
    assert(cluster.route_reparent_total == 4U);
    printf("[failover] scenario A pass\n");
}

static void scenario_dual_relay_drop(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay2;
    suite_member_t *relay3;
    suite_member_t *relay5;
    suite_member_t *relay6;

    suite_seed_cluster(&cluster);
    relay2 = suite_find_member(&cluster, 2U);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    relay6 = suite_find_member(&cluster, 6U);
    assert(relay2 != NULL && relay3 != NULL && relay5 != NULL && relay6 != NULL);

    relay2->online = 0U;
    relay3->online = 0U;
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);

    assert(relay2->relay_allowed == 0U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);
    assert(relay6->relay_allowed != 0U);
    assert(suite_count_online_relays(&cluster) == 3U);
    printf("[failover] scenario B pass\n");
}

static void scenario_send_config_fail_retry(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay3;
    suite_member_t *relay5;

    suite_seed_cluster(&cluster);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    relay3->online = 0U;
    suite_refresh_online_seen(&cluster, 108U);
    cluster.inject_send_fail_member_id = 5U;
    cluster.inject_send_fail_once = 1U;

    suite_rebalance_relays(&cluster, 108U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed == 0U);
    assert(suite_count_online_relays(&cluster) == 2U);
    assert(cluster.relay_target_count == 3U);

    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(relay5->relay_allowed != 0U);
    assert(suite_count_online_relays(&cluster) == 3U);
    printf("[failover] scenario C pass\n");
}

static void scenario_hidden_relay_failover_race(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay3;
    suite_member_t *relay5;

    suite_seed_cluster(&cluster);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    cluster.pairing_enabled = 1U;
    cluster.pairing_hidden_relay_only = 1U;
    suite_leaf_reselect_parent(&cluster, 10U, 3U);
    assert(suite_leaf_can_route_msg(&cluster, 10U, SLE_TEAM_APP_HELLO) != 0U);
    assert(suite_leaf_can_route_msg(&cluster, 10U, SLE_TEAM_APP_ROUTE_UPDATE) != 0U);
    assert(suite_leaf_can_route_msg(&cluster, 10U, SLE_TEAM_APP_POS_REPORT) == 0U);

    relay3->online = 0U;
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(relay3->relay_allowed != 0U);
    assert(relay5->relay_allowed == 0U);

    cluster.pairing_enabled = 0U;
    cluster.pairing_hidden_relay_only = 0U;
    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);
    suite_leaf_reselect_parent(&cluster, 10U, 5U);
    assert(suite_leaf_can_route_msg(&cluster, 10U, SLE_TEAM_APP_POS_REPORT) != 0U);
    printf("[failover] scenario G pass\n");
}

static void scenario_pairing_boundary_race(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay3;
    suite_member_t *relay5;

    suite_seed_cluster(&cluster);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    cluster.pairing_enabled = 1U;
    relay3->online = 0U;
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(relay3->relay_allowed != 0U);
    assert(relay5->relay_allowed == 0U);

    cluster.pairing_enabled = 0U;
    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);
    printf("[failover] scenario D pass\n");
}

static void scenario_target_downscale_demote(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay2;
    suite_member_t *relay3;
    suite_member_t *relay4;

    suite_seed_cluster(&cluster);
    relay2 = suite_find_member(&cluster, 2U);
    relay3 = suite_find_member(&cluster, 3U);
    relay4 = suite_find_member(&cluster, 4U);
    assert(relay2 != NULL && relay3 != NULL && relay4 != NULL);

    suite_set_online_count(&cluster, 17U);
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(cluster.relay_target_count == 3U);
    assert(cluster.relay_online_count == 3U);

    suite_set_online_count(&cluster, 16U);
    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(cluster.relay_target_count == 2U);
    assert(cluster.relay_online_count == 2U);
    assert(relay4->relay_allowed == 0U);
    assert(relay2->relay_allowed != 0U);
    assert(relay3->relay_allowed != 0U);
    printf("[failover] scenario H pass\n");
}

static void scenario_demoted_relay_leaf_reparent(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay2;
    suite_member_t *relay3;
    suite_member_t *relay4;

    suite_seed_cluster(&cluster);
    relay2 = suite_find_member(&cluster, 2U);
    relay3 = suite_find_member(&cluster, 3U);
    relay4 = suite_find_member(&cluster, 4U);
    assert(relay2 != NULL && relay3 != NULL && relay4 != NULL);

    suite_leaf_reselect_parent(&cluster, 10U, 4U);
    suite_leaf_reselect_parent(&cluster, 11U, 4U);
    assert(suite_leaf_can_report(&cluster, 10U) != 0U);
    assert(suite_leaf_can_report(&cluster, 11U) != 0U);

    suite_set_online_count(&cluster, 17U);
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(cluster.relay_online_count == 3U);

    suite_set_online_count(&cluster, 16U);
    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(relay4->relay_allowed == 0U);
    assert(suite_leaf_can_report(&cluster, 10U) == 0U);
    assert(suite_leaf_can_report(&cluster, 11U) == 0U);

    suite_leaf_reselect_parent(&cluster, 10U, 2U);
    suite_leaf_reselect_parent(&cluster, 11U, 3U);
    assert(suite_leaf_can_report(&cluster, 10U) != 0U);
    assert(suite_leaf_can_report(&cluster, 11U) != 0U);
    printf("[failover] scenario J pass\n");
}

static void scenario_unknown_rssi_demote(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay2;
    suite_member_t *relay3;
    suite_member_t *relay4;

    suite_seed_cluster(&cluster);
    relay2 = suite_find_member(&cluster, 2U);
    relay3 = suite_find_member(&cluster, 3U);
    relay4 = suite_find_member(&cluster, 4U);
    assert(relay2 != NULL && relay3 != NULL && relay4 != NULL);

    relay3->last_rssi_dbm = SLE_TEAM_RSSI_UNKNOWN;
    relay4->last_rssi_dbm = -75;
    suite_set_online_count(&cluster, 17U);
    suite_refresh_online_seen(&cluster, 108U);
    suite_rebalance_relays(&cluster, 108U);
    assert(cluster.relay_online_count == 3U);

    suite_set_online_count(&cluster, 16U);
    suite_refresh_online_seen(&cluster, 116U);
    suite_rebalance_relays(&cluster, 116U);
    assert(cluster.relay_target_count == 2U);
    assert(relay3->relay_allowed == 0U);
    assert(relay2->relay_allowed != 0U);
    assert(relay4->relay_allowed != 0U);
    printf("[failover] scenario K pass\n");
}

static void scenario_threshold_flap(void)
{
    suite_cluster_t cluster;
    const uint8_t online_seq[] = {8U, 9U, 8U, 17U, 16U, 17U};
    const uint8_t target_seq[] = {0U, 2U, 0U, 3U, 2U, 3U};
    size_t i;
    uint32_t now_s = 108U;

    suite_seed_cluster(&cluster);
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        cluster.members[i].relay_allowed = 0U;
    }

    for (i = 0U; i < sizeof(online_seq); i++) {
        suite_set_online_count(&cluster, online_seq[i]);
        suite_refresh_online_seen(&cluster, now_s);
        suite_rebalance_relays(&cluster, now_s);
        assert(cluster.relay_target_count == target_seq[i]);
        assert(cluster.relay_online_count <= SUITE_AUTO_RELAY_MAX);
        now_s = (uint32_t)(now_s + SUITE_REBALANCE_INTERVAL_S);
    }
    printf("[failover] scenario E pass\n");
}

static void scenario_leader_restart_reconcile(void)
{
    suite_cluster_t before;
    suite_cluster_t after;
    uint8_t i;

    suite_seed_cluster(&before);
    suite_set_online_count(&before, 16U);
    suite_refresh_online_seen(&before, 108U);
    suite_rebalance_relays(&before, 108U);
    assert(before.relay_online_count == 2U);
    assert(before.relay_target_count == 2U);
    before.pairing_enabled = 1U;
    before.pairing_hidden_relay_only = 1U;

    suite_seed_cluster(&after);
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        after.members[i].online = before.members[i].online;
        after.members[i].last_seen_s = before.members[i].last_seen_s;
        after.members[i].last_rssi_dbm = before.members[i].last_rssi_dbm;
        after.members[i].relay_allowed = 0U;
    }
    after.relay_rebalance_last_s = 0U;
    assert(after.pairing_enabled == 0U);
    assert(after.pairing_hidden_relay_only == 0U);
    suite_rebalance_relays(&after, 108U);
    assert(after.relay_target_count == before.relay_target_count);
    assert(after.relay_online_count == before.relay_online_count);
    printf("[failover] scenario I pass\n");
}

static void scenario_wraparound_time(void)
{
    suite_cluster_t cluster;
    suite_member_t *relay3;
    suite_member_t *relay5;

    suite_seed_cluster(&cluster);
    relay3 = suite_find_member(&cluster, 3U);
    relay5 = suite_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    cluster.relay_rebalance_last_s = UINT32_MAX - 2U;
    relay3->last_seen_s = UINT32_MAX - 15U;
    relay5->last_seen_s = UINT32_MAX - 1U;

    suite_rebalance_relays(&cluster, 3U);
    assert(relay3->relay_allowed != 0U);

    suite_rebalance_relays(&cluster, 7U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);
    printf("[failover] scenario F pass\n");
}

int main(void)
{
    scenario_single_relay_drop_leaf_recover();
    scenario_dual_relay_drop();
    scenario_send_config_fail_retry();
    scenario_hidden_relay_failover_race();
    scenario_pairing_boundary_race();
    scenario_target_downscale_demote();
    scenario_demoted_relay_leaf_reparent();
    scenario_unknown_rssi_demote();
    scenario_threshold_flap();
    scenario_leader_restart_reconcile();
    scenario_wraparound_time();
    printf("[failover] all scenarios passed\n");
    return 0;
}
