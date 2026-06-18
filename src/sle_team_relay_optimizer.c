#include "sle_team_relay_optimizer.h"

#include <stddef.h>

#define SLE_TEAM_OPT_RSSI_MIN_DBM (-92)
#define SLE_TEAM_OPT_RSSI_HYSTERESIS_DB 12
#define SLE_TEAM_OPT_RELAY_CHILD_CAP 7U

/* Route id 0 and broadcast are not real member records. */
static uint8_t opt_valid_member_id(uint8_t member_id)
{
    return (uint8_t)(member_id != 0U && member_id != SLE_TEAM_BROADCAST_ID);
}

/* Unknown RSSI is deliberately worse than every usable measured RSSI. */
static int8_t opt_rssi_score(int8_t rssi_dbm)
{
    return rssi_dbm == SLE_TEAM_RSSI_UNKNOWN ? (int8_t)-128 : rssi_dbm;
}

/* Wrap-safe elapsed check for second counters. */
static uint8_t opt_elapsed_exceeds(uint32_t now_s, uint32_t mark_s, uint32_t limit_s)
{
    if (mark_s == 0U || limit_s == 0U) {
        return 1U;
    }
    return (uint8_t)((uint32_t)(now_s - mark_s) > limit_s ? 1U : 0U);
}

/* Only leader-direct nodes are safe candidates for this first optimizer. */
static uint8_t opt_is_leader_direct(const sle_team_node_t *node, const sle_team_member_record_t *member)
{
    if (node == NULL || member == NULL) {
        return 0U;
    }
    return (uint8_t)(member->parent_id == node->cfg.self_id || member->parent_id == node->cfg.leader_id);
}

/* Pairing/allow-list gaps mean the group is still changing, so do not optimize. */
static uint8_t opt_allowed_member_missing(const sle_team_node_t *node)
{
    uint8_t i;

    if (node->cfg.member_filter_enabled == 0U || node->cfg.allowed_member_count == 0U) {
        return 0U;
    }
    for (i = 0U; i < node->cfg.allowed_member_count; i++) {
        const sle_team_member_record_t *member =
            sle_team_node_find_member(node, node->cfg.allowed_member_ids[i]);

        if (member == NULL || member->online == 0U) {
            return 1U;
        }
    }
    return 0U;
}

/* Any recovery, pending policy, or half-offline record makes the table unstable. */
static uint8_t opt_has_unstable_member(const sle_team_node_t *node)
{
    uint8_t i;

    if (node->relay_recovery_pending != 0U || node->cfg.pairing_enabled != 0U ||
        opt_allowed_member_missing(node) != 0U) {
        return 1U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];

        if (opt_valid_member_id(member->member_id) == 0U) {
            continue;
        }
        if (member->policy_pending != 0U || member->relay_recovery_candidate != 0U) {
            return 1U;
        }
        if (member->online == 0U && (member->parent_id != 0U || member->next_hop_id != 0U)) {
            return 1U;
        }
    }
    return 0U;
}

/* First production pass only handles exactly one childless leader-direct relay. */
static uint8_t opt_has_complex_relay_topology(const sle_team_node_t *node)
{
    uint8_t i;
    uint8_t relay_count = 0U;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];

        if (opt_valid_member_id(member->member_id) == 0U || member->online == 0U ||
            member->relay_allowed == 0U) {
            continue;
        }
        relay_count++;
        if (member->child_count != 0U || opt_is_leader_direct(node, member) == 0U) {
            return 1U;
        }
    }
    return (uint8_t)(relay_count == 1U ? 0U : 1U);
}

/* Best replacement: online, leader-direct, recent, non-relay, strong RSSI. */
static sle_team_member_record_t *opt_pick_best_candidate(sle_team_node_t *node, uint32_t now_s, uint32_t timeout_s)
{
    uint8_t i;
    sle_team_member_record_t *best = NULL;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        sle_team_member_record_t *member = &node->members[i];

        if (opt_valid_member_id(member->member_id) == 0U || member->online == 0U ||
            member->policy_pending != 0U || member->relay_allowed != 0U ||
            member->relay_recovery_candidate != 0U || opt_is_leader_direct(node, member) == 0U ||
            member->child_count != 0U || member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN ||
            member->last_rssi_dbm < SLE_TEAM_OPT_RSSI_MIN_DBM ||
            opt_elapsed_exceeds(now_s, member->last_seen_s, timeout_s) != 0U) {
            continue;
        }
        if (best == NULL || member->last_rssi_dbm > best->last_rssi_dbm ||
            (member->last_rssi_dbm == best->last_rssi_dbm && member->member_id < best->member_id)) {
            best = member;
        }
    }
    return best;
}

/* Worst current relay, but only if it has no children to strand. */
static sle_team_member_record_t *opt_pick_worst_childless_relay(sle_team_node_t *node, uint32_t now_s,
    uint32_t timeout_s)
{
    uint8_t i;
    sle_team_member_record_t *worst = NULL;
    int8_t worst_score = 127;
    uint32_t worst_age_s = 0U;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        sle_team_member_record_t *member = &node->members[i];
        int8_t score;
        uint32_t age_s;

        if (opt_valid_member_id(member->member_id) == 0U || member->online == 0U ||
            member->policy_pending != 0U || member->relay_allowed == 0U ||
            member->relay_recovery_candidate != 0U || opt_is_leader_direct(node, member) == 0U ||
            member->child_count != 0U || opt_elapsed_exceeds(now_s, member->last_seen_s, timeout_s) != 0U) {
            continue;
        }
        score = opt_rssi_score(member->last_rssi_dbm);
        age_s = (uint32_t)(now_s - member->last_seen_s);
        if (worst == NULL || score < worst_score ||
            (score == worst_score && age_s > worst_age_s) ||
            (score == worst_score && age_s == worst_age_s && member->member_id > worst->member_id)) {
            worst = member;
            worst_score = score;
            worst_age_s = age_s;
        }
    }
    return worst;
}

int sle_team_relay_optimizer_run(sle_team_node_t *node, uint32_t now_s)
{
    uint32_t timeout_s;
    sle_team_member_record_t *candidate;
    sle_team_member_record_t *victim;
    int8_t candidate_score;
    int8_t victim_score;

    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return SLE_TEAM_ERR_ARG;
    }
    if (opt_has_unstable_member(node) != 0U || opt_has_complex_relay_topology(node) != 0U) {
        return SLE_TEAM_OK;
    }
    timeout_s = node->cfg.heartbeat_timeout_s != 0U ? node->cfg.heartbeat_timeout_s : 3U;
    candidate = opt_pick_best_candidate(node, now_s, timeout_s);
    victim = opt_pick_worst_childless_relay(node, now_s, timeout_s);
    if (candidate == NULL || victim == NULL || candidate->member_id == victim->member_id) {
        return SLE_TEAM_OK;
    }
    candidate_score = opt_rssi_score(candidate->last_rssi_dbm);
    victim_score = opt_rssi_score(victim->last_rssi_dbm);
    /* Hysteresis prevents rapid relay flapping on small RSSI changes. */
    if (candidate_score < (int8_t)(victim_score + SLE_TEAM_OPT_RSSI_HYSTERESIS_DB)) {
        return SLE_TEAM_OK;
    }
    /* Swap policy in the leader table first, then push CONFIG/ROUTE_UPDATE. */
    candidate->relay_allowed = 1U;
    candidate->relay_tier = 1U;
    candidate->max_downstream = SLE_TEAM_OPT_RELAY_CHILD_CAP;
    victim->relay_allowed = 0U;
    victim->relay_tier = 0U;
    victim->max_downstream = 0U;
    (void)sle_team_node_send_config(node, candidate->member_id);
    (void)sle_team_node_send_route_update(node, candidate->member_id, node->cfg.leader_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, node->cfg.leader_id);
    (void)sle_team_node_send_config(node, victim->member_id);
    (void)sle_team_node_send_route_update(node, victim->member_id, node->cfg.leader_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, node->cfg.leader_id);
    return 1;
}

uint8_t sle_team_relay_optimizer_tick(sle_team_node_t *node, uint32_t now_ms, uint32_t interval_ms,
    uint32_t *last_run_ms)
{
    if (node == NULL || last_run_ms == NULL || interval_ms == 0U) {
        return 0U;
    }
    if (*last_run_ms != 0U && (uint32_t)(now_ms - *last_run_ms) < interval_ms) {
        return 0U;
    }
    *last_run_ms = now_ms;
    return (uint8_t)(sle_team_relay_optimizer_run(node, now_ms / 1000U) > 0 ? 1U : 0U);
}
