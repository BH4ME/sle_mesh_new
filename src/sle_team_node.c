#include "sle_team_node.h"
#include <string.h>

/*
 * Portable leader/member state machine.
 *
 * The WS63 app owns radio connections, UART, display, GPS and logs. This file
 * owns only logical mesh policy: who is allowed in, who is direct, who becomes
 * a relay, which parent a member should use, and how recovery is confirmed.
 */
static const uint8_t g_zero_cipher_mac[2] = {0x00, 0x00};
#define SLE_TEAM_DIRECT_CAP_DEFAULT 7U
#define SLE_TEAM_RELAY_CHILD_CAP_DEFAULT 7U
#define SLE_TEAM_MEMBER_HELLO_INTERVAL_S 1U
static int sle_team_send_leader_heartbeat(sle_team_node_t *node, uint8_t member_id);

/* Route id 0 and broadcast are control values, not real member ids. */
static uint8_t sle_team_valid_member_id(uint8_t member_id)
{
    return (uint8_t)(member_id != 0U && member_id != SLE_TEAM_BROADCAST_ID);
}

/* Small callback wrappers keep the state machine portable and testable. */
static uint32_t sle_team_now(const sle_team_node_t *node)
{
    if (node == NULL || node->ops.now_s == NULL) { return 0U; }
    return node->ops.now_s(node->ops.user_ctx);
}
static uint8_t sle_team_battery_percent(const sle_team_node_t *node)
{
    if (node == NULL || node->ops.battery_percent == NULL) { return 100U; }
    return node->ops.battery_percent(node->ops.user_ctx);
}
static int8_t sle_team_rssi_dbm(const sle_team_node_t *node)
{
    if (node == NULL || node->ops.rssi_dbm == NULL) { return SLE_TEAM_RSSI_UNKNOWN; }
    return node->ops.rssi_dbm(node->ops.user_ctx);
}
static void sle_team_log(const sle_team_node_t *node, const char *text)
{
    if (node != NULL && node->ops.log != NULL) {
        node->ops.log(node->ops.user_ctx, text);
    }
}
static void sle_team_mark_joined(sle_team_node_t *node, uint8_t member_id)
{
    if (node != NULL && node->ops.on_joined != NULL) {
        node->ops.on_joined(node->ops.user_ctx, member_id);
    }
}

/* Leader direct capacity is capped below the physical connection table size. */
static uint8_t sle_team_leader_direct_cap(const sle_team_node_t *node)
{
    uint8_t cap = SLE_TEAM_DIRECT_CAP_DEFAULT;
    if (node != NULL && node->cfg.role == SLE_TEAM_ROLE_LEADER && node->cfg.max_downstream != 0U) {
        cap = node->cfg.max_downstream;
    }
    if (cap >= SLE_TEAM_MAX_DIRECT_CONNECTIONS) {
        cap = (uint8_t)(SLE_TEAM_MAX_DIRECT_CONNECTIONS - 1U);
    }
    if (cap == 0U) { cap = 1U; }
    return cap;
}

/* Relay child capacity defaults to the same conservative cap unless configured. */
static uint8_t sle_team_relay_child_cap(const sle_team_node_t *node)
{
    if (node != NULL && node->cfg.role == SLE_TEAM_ROLE_MEMBER && node->cfg.max_downstream != 0U) {
        return node->cfg.max_downstream;
    }
    return SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
}

/* Member records are stable slots keyed by logical route id. */
static const sle_team_member_record_t *sle_team_find_member_const(const sle_team_node_t *node, uint8_t member_id)
{
    uint8_t i;
    if (node == NULL || sle_team_valid_member_id(member_id) == 0U) { return NULL; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->members[i].member_id == member_id) {
            return &node->members[i];
        }
    }
    return NULL;
}
static sle_team_member_record_t *sle_team_find_member(sle_team_node_t *node, uint8_t member_id)
{
    return (sle_team_member_record_t *)sle_team_find_member_const(node, member_id);
}
static sle_team_member_record_t *sle_team_member_slot(sle_team_node_t *node, uint8_t member_id, uint8_t create)
{
    uint8_t i;
    sle_team_member_record_t *free_slot = NULL;
    sle_team_member_record_t *member;
    if (node == NULL || sle_team_valid_member_id(member_id) == 0U) {
        return NULL;
    }
    member = sle_team_find_member(node, member_id);
    if (member != NULL) { return member; }
    if (create == 0U) { return NULL; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->members[i].member_id == 0U) {
            free_slot = &node->members[i];
            break;
        }
    }
    if (free_slot == NULL) { return NULL; }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->member_id = member_id;
    free_slot->role = SLE_TEAM_ROLE_MEMBER;
    free_slot->last_rssi_dbm = SLE_TEAM_RSSI_UNKNOWN;
    return free_slot;
}

/* Pending records are HELLOs waiting for allow-list/pairing approval. */
static sle_team_pending_member_t *sle_team_pending_slot(sle_team_node_t *node, uint8_t member_id, uint8_t create)
{
    uint8_t i;
    sle_team_pending_member_t *free_slot = NULL;
    if (node == NULL || sle_team_valid_member_id(member_id) == 0U) { return NULL; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->pending_members[i].active != 0U && node->pending_members[i].member_id == member_id) {
            return &node->pending_members[i];
        }
        if (free_slot == NULL && node->pending_members[i].active == 0U) {
            free_slot = &node->pending_members[i];
        }
    }
    if (create == 0U || free_slot == NULL) { return NULL; }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->member_id = member_id;
    free_slot->active = 1U;
    return free_slot;
}
static void sle_team_clear_pending(sle_team_node_t *node, uint8_t member_id)
{
    uint8_t i;
    if (node == NULL) { return; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->pending_members[i].active != 0U && node->pending_members[i].member_id == member_id) {
            (void)memset(&node->pending_members[i], 0, sizeof(node->pending_members[i]));
        }
    }
}
static uint8_t sle_team_id_in_list(const uint8_t *member_ids, uint8_t count, uint8_t member_id)
{
    uint8_t i;
    if (member_ids == NULL || sle_team_valid_member_id(member_id) == 0U) { return 0U; }
    for (i = 0U; i < count; i++) {
        if (member_ids[i] == member_id) {
            return 1U;
        }
    }
    return 0U;
}
uint8_t sle_team_node_is_member_allowed(const sle_team_node_t *node, uint8_t member_id)
{
    if (node == NULL || sle_team_valid_member_id(member_id) == 0U) { return 0U; }
    if (node->cfg.role != SLE_TEAM_ROLE_LEADER || node->cfg.member_filter_enabled == 0U) { return 1U; }
    if (node->cfg.allowed_member_count == 0U) { return 0U; }
    return sle_team_id_in_list(node->cfg.allowed_member_ids, node->cfg.allowed_member_count, member_id);
}

/* Recompute derived child counts after parent/online/pending state changes. */
static void sle_team_recompute_child_counts(sle_team_node_t *node)
{
    uint8_t i;
    uint8_t j;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) { return; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        node->members[i].child_count = 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        sle_team_member_record_t *child = &node->members[i];
        if ((child->online == 0U && child->policy_pending == 0U) || sle_team_valid_member_id(child->member_id) == 0U ||
            child->parent_id == 0U || child->parent_id == node->cfg.self_id ||
            child->parent_id == node->cfg.leader_id || child->parent_id == child->member_id) {
            continue;
        }
        for (j = 0U; j < SLE_TEAM_MAX_MEMBERS; j++) {
            sle_team_member_record_t *parent = &node->members[j];
            if (parent->online != 0U && parent->member_id == child->parent_id && parent->child_count < 255U) {
                parent->child_count++;
                break;
            }
        }
    }
}

/* Direct count includes pending direct policy because it already reserves capacity. */
static uint8_t sle_team_direct_online_count(const sle_team_node_t *node)
{
    uint8_t i;
    uint8_t count = 0U;
    if (node == NULL) { return 0U; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        if ((member->online != 0U || member->policy_pending != 0U) &&
            sle_team_valid_member_id(member->member_id) != 0U &&
            (member->parent_id == node->cfg.self_id || member->parent_id == node->cfg.leader_id)) {
            count++;
        }
    }
    return count;
}

/* Pick the best online leader-direct relay with free downstream capacity. */
static uint8_t sle_team_select_online_relay(const sle_team_node_t *node, uint8_t exclude_id)
{
    uint8_t i;
    uint8_t best_id = 0U;
    uint8_t best_free_slots = 0U;
    int8_t best_rssi = (int8_t)-128;
    uint8_t best_child_count = 0xFFU;
    if (node == NULL) { return 0U; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        uint8_t cap;
        if (member->online == 0U || member->relay_allowed == 0U || member->member_id == exclude_id ||
            sle_team_valid_member_id(member->member_id) == 0U) {
            continue;
        }
        if (member->parent_id != node->cfg.self_id && member->parent_id != node->cfg.leader_id) { continue; }
        cap = member->max_downstream != 0U ? member->max_downstream : SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
        if (member->child_count >= cap) { continue; }
        if (best_id == 0U) {
            best_id = member->member_id;
            best_free_slots = (uint8_t)(cap - member->child_count);
            best_rssi = member->last_rssi_dbm;
            best_child_count = member->child_count;
            continue;
        }
        {
            uint8_t free_slots = (uint8_t)(cap - member->child_count);
            if (free_slots > best_free_slots ||
                (free_slots == best_free_slots && member->last_rssi_dbm > best_rssi) ||
                (free_slots == best_free_slots && member->last_rssi_dbm == best_rssi &&
                member->child_count < best_child_count) ||
                (free_slots == best_free_slots && member->last_rssi_dbm == best_rssi &&
                member->child_count == best_child_count && member->member_id < best_id)) {
                best_id = member->member_id;
                best_free_slots = free_slots;
                best_rssi = member->last_rssi_dbm;
                best_child_count = member->child_count;
            }
        }
    }
    return best_id;
}

/* A forwarded child HELLO may prefer the relay that physically delivered it. */
static uint8_t sle_team_ingress_relay_can_parent(const sle_team_node_t *node, uint8_t relay_id,
    uint8_t child_id)
{
    const sle_team_member_record_t *relay;
    uint8_t cap;
    if (node == NULL || relay_id == 0U || relay_id == child_id ||
        relay_id == node->cfg.self_id || relay_id == node->cfg.leader_id) {
        return 0U;
    }
    relay = sle_team_find_member_const(node, relay_id);
    if (relay == NULL || relay->online == 0U || relay->relay_allowed == 0U) {
        return 0U;
    }
    if (relay->parent_id != node->cfg.self_id && relay->parent_id != node->cfg.leader_id) {
        return 0U;
    }
    cap = relay->max_downstream != 0U ? relay->max_downstream : SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
    return relay->child_count < cap ? 1U : 0U;
}

/* Runtime relay grant chooses an already online leader-direct member. */
static sle_team_member_record_t *sle_team_select_relay_grant_candidate(sle_team_node_t *node)
{
    uint8_t i;
    sle_team_member_record_t *best = NULL;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) { return NULL; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        sle_team_member_record_t *member = &node->members[i];
        if (member->online == 0U || member->relay_allowed != 0U || member->member_id == node->relay_recovery_lost_relay_id ||
            sle_team_valid_member_id(member->member_id) == 0U) {
            continue;
        }
        if (member->parent_id != node->cfg.self_id && member->parent_id != node->cfg.leader_id) {
            continue;
        }
        if (best == NULL ||
            member->last_rssi_dbm > best->last_rssi_dbm ||
            (member->last_rssi_dbm == best->last_rssi_dbm && member->child_count < best->child_count) ||
            (member->last_rssi_dbm == best->last_rssi_dbm && member->child_count == best->child_count &&
            member->member_id < best->member_id)) {
            best = member;
        }
    }
    return best;
}

/* Ensure overflow has a usable relay before assigning a new child. */
static uint8_t sle_team_ensure_relay_for_overflow(sle_team_node_t *node, uint8_t exclude_id)
{
    sle_team_member_record_t *candidate;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) { return 0U; }
    if (sle_team_select_online_relay(node, exclude_id) != 0U) { return 1U; }
    candidate = sle_team_select_relay_grant_candidate(node);
    if (candidate == NULL) { return 0U; }
    return sle_team_node_grant_relay(node, candidate->member_id) == SLE_TEAM_OK ? 1U : 0U;
}
static uint8_t sle_team_has_relay_recovery_candidate(const sle_team_node_t *node)
{
    uint8_t i; if (node == NULL) { return 0U; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        if (member->relay_recovery_candidate != 0U && sle_team_valid_member_id(member->member_id) != 0U &&
            member->member_id != node->relay_recovery_lost_relay_id) { return 1U; }
    }
    return 0U;
}

/* Pick the freshest recovery candidate, using RSSI/member id only as tie-breakers. */
static uint8_t sle_team_select_relay_recovery_candidate_id(const sle_team_node_t *node)
{
    uint8_t i, best_id = 0U;
    uint32_t best_last_seen = 0U; int8_t best_rssi = (int8_t)-128;
    if (node == NULL) { return 0U; }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        if (member->relay_recovery_candidate == 0U || sle_team_valid_member_id(member->member_id) == 0U ||
            member->member_id == node->relay_recovery_lost_relay_id) { continue; }
        if (best_id == 0U ||
            member->last_seen_s > best_last_seen ||
            (member->last_seen_s == best_last_seen && member->last_rssi_dbm > best_rssi) ||
            (member->last_seen_s == best_last_seen && member->last_rssi_dbm == best_rssi &&
            member->member_id < best_id)) {
            best_id = member->member_id;
            best_last_seen = member->last_seen_s;
            best_rssi = member->last_rssi_dbm;
        }
    }
    return best_id;
}

/* During relay recovery, keep using the selected replacement relay if it is
 * still online. This avoids flip-flopping children between candidate relays. */
static void sle_team_choose_relay_recovery_candidate(sle_team_node_t *node)
{
    const sle_team_member_record_t *selected;
    if (node == NULL) { return; }
    selected = sle_team_find_member_const(node, node->relay_recovery_selected_id);
    if (selected != NULL && selected->online != 0U && selected->relay_allowed != 0U &&
        selected->member_id != node->relay_recovery_lost_relay_id) { return; }
    node->relay_recovery_selected_id = sle_team_select_relay_recovery_candidate_id(node);
}
static void sle_team_grant_recovery_relay_policy(sle_team_node_t *node, sle_team_member_record_t *member)
{
    /* Replacement relay is promoted under leader authority before children move. */
    if (node == NULL || member == NULL) { return; }
    member->relay_allowed = 1U;
    member->relay_tier = 1U;
    member->max_downstream = SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
    node->relay_recovery_selected_id = member->member_id;
}
static void sle_team_finish_relay_recovery_if_done(sle_team_node_t *node)
{
    uint8_t i, lost_id;
    if (node == NULL || node->relay_recovery_pending == 0U) { return; }
    lost_id = node->relay_recovery_lost_relay_id;
    /* Recovery is not finished until the lost relay has lost relay authority
     * and no remaining child still points at it as parent. */
    for (i = 0U; lost_id != 0U && i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        if (member->member_id == lost_id && member->relay_allowed != 0U) { return; }
        if (member->member_id != lost_id && member->parent_id == lost_id) { return; }
    }
    if (sle_team_has_relay_recovery_candidate(node) == 0U) {
        node->relay_recovery_pending = 0U;
        node->relay_recovery_selected_id = 0U;
    }
}
int sle_team_node_grant_relay(sle_team_node_t *node, uint8_t member_id)
{
    sle_team_member_record_t *member;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER || sle_team_valid_member_id(member_id) == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    member = sle_team_find_member(node, member_id);
    if (member == NULL || member->online == 0U || member_id == node->relay_recovery_lost_relay_id) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (member->parent_id != node->cfg.self_id && member->parent_id != node->cfg.leader_id) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    member->relay_allowed = 1U;
    member->relay_tier = 1U;
    member->max_downstream = SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
    (void)sle_team_node_send_config(node, member->member_id);
    (void)sle_team_node_send_route_update(node, member->member_id, node->cfg.leader_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, node->cfg.leader_id);
    sle_team_recompute_child_counts(node);
    sle_team_log(node, "relay granted");
    return SLE_TEAM_OK;
}

/*
 * Decide the leader-authoritative parent for a member.
 *
 * Normal path: direct to leader while direct_cap has room, otherwise child of a
 * leader-direct relay. Recovery path: never let the lost relay reclaim relay
 * authority while replacement relays/children are still being settled.
 */
static uint8_t sle_team_assign_parent(sle_team_node_t *node, sle_team_member_record_t *member)
{
    uint8_t direct_cap;
    uint8_t direct_count;
    uint8_t parent_id;
    uint8_t recovery_parent;
    const sle_team_member_record_t *selected;
    if (node == NULL || member == NULL) {
        return 0U;
    }
    sle_team_recompute_child_counts(node);
    /* A relay that just disappeared is treated specially when it comes back:
     * it must rejoin as an ordinary child under the current replacement relay,
     * not reclaim stale relay status. */
    if (node->relay_recovery_lost_relay_id != 0U &&
        member->member_id == node->relay_recovery_lost_relay_id) {
        recovery_parent = sle_team_select_online_relay(node, member->member_id);
        if (recovery_parent == 0U) {
            if (node->relay_recovery_pending != 0U) {
                member->parent_id = 0U;
                member->next_hop_id = 0U;
                sle_team_recompute_child_counts(node);
                return 0U;
            }
            node->relay_recovery_lost_relay_id = 0U;
        } else {
            member->relay_allowed = 0U;
            member->relay_tier = 0U;
            member->max_downstream = 0U;
            member->parent_id = recovery_parent;
            member->next_hop_id = recovery_parent;
            sle_team_recompute_child_counts(node);
            return recovery_parent;
        }
    }
    if (node->relay_recovery_pending != 0U) {
        selected = sle_team_find_member_const(node, node->relay_recovery_selected_id);
        /* While recovery is pending, one replacement relay is promoted first.
         * Other affected members wait or become children of that relay. */
        if (member->relay_recovery_candidate != 0U &&
            selected != NULL && selected->online != 0U && selected->relay_allowed != 0U &&
            node->relay_recovery_selected_id != member->member_id) {
            member->relay_allowed = 0U;
            member->relay_tier = 0U;
            member->max_downstream = 0U;
            member->parent_id = node->relay_recovery_selected_id;
            member->next_hop_id = node->relay_recovery_selected_id;
            sle_team_recompute_child_counts(node);
            return member->parent_id;
        }
        if (member->relay_recovery_candidate != 0U &&
            (node->relay_recovery_selected_id == 0U ||
            selected == NULL || (selected->online == 0U && selected->policy_pending == 0U))) {
            node->relay_recovery_selected_id = member->member_id;
        } else if (node->relay_recovery_selected_id == 0U &&
            sle_team_has_relay_recovery_candidate(node) != 0U) {
            sle_team_choose_relay_recovery_candidate(node);
        }
        if (node->relay_recovery_selected_id == member->member_id) {
            sle_team_grant_recovery_relay_policy(node, member);
            member->parent_id = node->cfg.leader_id;
            member->next_hop_id = node->cfg.leader_id;
            sle_team_recompute_child_counts(node);
            return member->parent_id;
        }
        member->parent_id = 0U;
        member->next_hop_id = 0U;
        sle_team_recompute_child_counts(node);
        return 0U;
    }
    direct_cap = sle_team_leader_direct_cap(node);
    direct_count = sle_team_direct_online_count(node);
    if (direct_count < direct_cap) {
        parent_id = node->cfg.leader_id;
    } else {
        /* Direct capacity is full: make sure at least one direct member can act
         * as relay, then place the new member behind the best available relay. */
        if (sle_team_ensure_relay_for_overflow(node, member->member_id) == 0U) {
            member->parent_id = 0U;
            member->next_hop_id = 0U;
            sle_team_recompute_child_counts(node);
            return 0U;
        }
        parent_id = sle_team_select_online_relay(node, member->member_id);
    }
    member->parent_id = parent_id;
    member->next_hop_id = parent_id;
    sle_team_recompute_child_counts(node);
    return parent_id;
}
static void sle_team_clear_stale_join_policy(sle_team_member_record_t *member)
{
    if (member == NULL) {
        return;
    }
    member->relay_allowed = 0U;
    member->relay_tier = 0U;
    member->max_downstream = 0U;
    member->policy_pending = 0U;
    member->pending_ack_seq = 0U;
    member->parent_id = 0U;
    member->next_hop_id = 0U;
    member->child_count = 0U;
}

/* Send the two policy packets that make a member's parent assignment concrete. */
static int sle_team_send_member_policy(sle_team_node_t *node, sle_team_member_record_t *member, uint8_t mark_pending)
{
    if (node == NULL || member == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER ||
        sle_team_valid_member_id(member->member_id) == 0U || member->parent_id == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (member->next_hop_id == 0U) { member->next_hop_id = member->parent_id; }
    if (mark_pending != 0U) { member->policy_pending = 1U; }
    sle_team_recompute_child_counts(node);
    /* The route update is the actual parent decision. CONFIG follows with
     * timing/reporting/relay permission knobs for that same member. */
    uint16_t route_seq = node->next_seq;
    int ret = sle_team_node_send_route_update(node, member->member_id, member->parent_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, member->next_hop_id);
    if (ret != SLE_TEAM_OK) { return ret; }
    member->pending_ack_seq = route_seq;
    (void)sle_team_node_send_config(node, member->member_id);
    return SLE_TEAM_OK;
}
static uint8_t sle_team_recovery_policy_required(const sle_team_node_t *node,
    const sle_team_member_record_t *member)
{
    /*
     * Live traffic from a member can reveal that its current parent/relay flag
     * is stale. In that case the leader re-sends policy instead of accepting the
     * member online under a broken recovery topology.
     */
    const sle_team_member_record_t *selected;
    if (node == NULL || member == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) { return 0U; }
    if (node->relay_recovery_lost_relay_id != 0U &&
        member->member_id == node->relay_recovery_lost_relay_id) {
        return (uint8_t)(member->relay_allowed != 0U || member->parent_id == 0U ||
            member->parent_id == node->cfg.self_id || member->parent_id == node->cfg.leader_id);
    }
    if (node->relay_recovery_pending == 0U) { return 0U; }
    if (node->relay_recovery_lost_relay_id != 0U &&
        member->member_id != node->relay_recovery_lost_relay_id &&
        member->parent_id == node->relay_recovery_lost_relay_id) {
        return 1U;
    }
    if (member->relay_recovery_candidate == 0U) { return 0U; }
    if (node->relay_recovery_selected_id == member->member_id) {
        return (uint8_t)(member->relay_allowed == 0U ||
            (member->parent_id != node->cfg.self_id && member->parent_id != node->cfg.leader_id));
    }
    selected = sle_team_find_member_const(node, node->relay_recovery_selected_id);
    if (selected == NULL || selected->online == 0U || selected->relay_allowed == 0U) { return 0U; }
    return (uint8_t)(member->relay_allowed != 0U ||
        member->parent_id != node->relay_recovery_selected_id ||
        member->next_hop_id != node->relay_recovery_selected_id);
}
static int sle_team_refresh_recovery_policy(sle_team_node_t *node, sle_team_member_record_t *member)
{
    uint8_t member_id;
    uint8_t was_recovery_candidate;
    uint8_t was_lost_relay_child;
    if (node == NULL || member == NULL) { return SLE_TEAM_ERR_ARG; }
    member_id = member->member_id; was_recovery_candidate = member->relay_recovery_candidate;
    was_lost_relay_child = (uint8_t)(node->relay_recovery_lost_relay_id != 0U &&
        member_id != node->relay_recovery_lost_relay_id &&
        member->parent_id == node->relay_recovery_lost_relay_id);
    /* Keep the usable next hop if the member is already being recovered through
     * a live relay; only stale parent authority is cleared. */
    uint8_t preserved_next_hop = (uint8_t)(node->relay_recovery_selected_id == member_id &&
        member->next_hop_id != 0U && member->next_hop_id != node->relay_recovery_lost_relay_id &&
        member->next_hop_id != member_id ? member->next_hop_id : 0U);
    member->online = 0U;
    sle_team_clear_stale_join_policy(member);
    member->member_id = member_id;
    if (was_recovery_candidate != 0U || was_lost_relay_child != 0U) { member->relay_recovery_candidate = 1U; }
    if (was_lost_relay_child != 0U) { sle_team_choose_relay_recovery_candidate(node); }
    if (sle_team_assign_parent(node, member) == 0U) { return SLE_TEAM_ERR_UNSUPPORTED; }
    if (preserved_next_hop != 0U && member->parent_id == node->cfg.leader_id) { member->next_hop_id = preserved_next_hop; }
    return sle_team_send_member_policy(node, member, 1U);
}

/* Final confirmation point for HELLO ACK, ROUTE_UPDATE ACK, or coherent live packets. */
static int sle_team_confirm_member_online(sle_team_node_t *node, sle_team_member_record_t *member)
{
    uint8_t notify_joined;
    uint8_t was_recovery_relay;
    if (node == NULL || member == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER ||
        sle_team_valid_member_id(member->member_id) == 0U || member->parent_id == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    was_recovery_relay = (uint8_t)(member->relay_recovery_candidate != 0U &&
        node->relay_recovery_selected_id == member->member_id);
    notify_joined = (uint8_t)(member->online == 0U || member->policy_pending != 0U);
    member->online = 1U;
    member->policy_pending = 0U;
    member->pending_ack_seq = 0U;
    if (member->relay_recovery_candidate != 0U) { member->relay_recovery_candidate = 0U; }
    if (was_recovery_relay != 0U) { sle_team_log(node, "replacement relay online"); }
    if (member->last_seen_s == 0U) {
        member->last_seen_s = sle_team_now(node);
    }
    sle_team_recompute_child_counts(node);
    sle_team_finish_relay_recovery_if_done(node);
    if (notify_joined != 0U) {
        sle_team_mark_joined(node, member->member_id);
    }
    return SLE_TEAM_OK;
}
static uint8_t sle_team_pending_live_packet_confirms(const sle_team_node_t *node,
    const sle_team_member_record_t *member)
{
    if (node == NULL || member == NULL || member->policy_pending == 0U || member->parent_id == 0U) {
        return 0U;
    }
    if (member->next_hop_id == 0U) {
        return 0U;
    }
    /* A heartbeat/POS packet can confirm a pending route only when it arrived
     * through the same coherent next hop that the leader is trying to install. */
    return (uint8_t)(member->next_hop_id == member->parent_id ||
        member->next_hop_id == node->cfg.self_id ||
        member->next_hop_id == node->cfg.leader_id);
}

/* Re-send policy without destroying the existing ACK target on send failure. */
static int sle_team_resend_member_policy(sle_team_node_t *node, sle_team_member_record_t *member, uint16_t ack_seq)
{
    if (node == NULL || member == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER ||
        sle_team_valid_member_id(member->member_id) == 0U || member->parent_id == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (member->next_hop_id == 0U) {
        member->next_hop_id = member->parent_id;
    }
    if (sle_team_send_member_policy(node, member, 0U) != SLE_TEAM_OK) { return SLE_TEAM_ERR_UNSUPPORTED; }
    (void)sle_team_node_send_ack(node, member->member_id, ack_seq, SLE_TEAM_APP_HELLO, 0U);
    sle_team_clear_pending(node, member->member_id); return SLE_TEAM_OK;
}
static int sle_team_send_app(sle_team_node_t *node, uint8_t dst_id, const uint8_t *app_buf, uint16_t app_len)
{
    /* App packets are always wrapped into one GROUP_DATA mesh frame before TX. */
    sle_team_mesh_packet_t mesh_packet;
    uint8_t mesh_buf[SLE_TEAM_NODE_TX_BUF_SIZE];
    size_t mesh_len = 0U;
    if (node == NULL || node->ops.send == NULL || app_buf == NULL || app_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    if (sle_team_wrap_mesh_group_data(node->cfg.channel_hash, g_zero_cipher_mac, app_buf, app_len,
        dst_id == SLE_TEAM_BROADCAST_ID ? SLE_TEAM_ROUTE_FLOOD : SLE_TEAM_ROUTE_DIRECT,
        &mesh_packet) != SLE_TEAM_OK) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (sle_team_encode_mesh_packet(&mesh_packet, mesh_buf, sizeof(mesh_buf), &mesh_len) != SLE_TEAM_OK) {
        return SLE_TEAM_ERR_BUF;
    }
    return node->ops.send(node->ops.user_ctx,
        dst_id == SLE_TEAM_BROADCAST_ID ? SLE_TEAM_SEND_GROUP : SLE_TEAM_SEND_UNICAST,
        dst_id, mesh_buf, (uint16_t)mesh_len);
}
static int sle_team_send_encoded_packet(sle_team_node_t *node, uint8_t dst_id, const sle_team_app_packet_t *app_packet)
{
    uint8_t app_buf[SLE_TEAM_MAX_PAYLOAD_SIZE];
    uint16_t app_len = 0U;
    if (node == NULL || app_packet == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    if (sle_team_encode_app_packet(app_packet, app_buf, sizeof(app_buf), &app_len) != SLE_TEAM_OK) {
        return SLE_TEAM_ERR_BUF;
    }
    return sle_team_send_app(node, dst_id, app_buf, app_len);
}
static int sle_team_build_and_send(sle_team_node_t *node, uint8_t dst_id, uint8_t msg_type,
    const uint8_t *body, uint16_t body_len)
{
    /* Single sequence counter per node makes ACK/pending-policy matching simple. */
    sle_team_app_packet_t app_packet;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    app_packet.app_msg_type = msg_type;
    app_packet.flags = 0U;
    app_packet.seq = node->next_seq++;
    app_packet.team_id = node->cfg.team_id;
    app_packet.src_id = node->cfg.self_id;
    app_packet.dst_id = dst_id;
    app_packet.ttl = node->cfg.default_ttl != 0U ? node->cfg.default_ttl : 1U;
    app_packet.leader_term = node->cfg.leader_term != 0U ? node->cfg.leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    app_packet.body_len = body_len;
    app_packet.body = body;
    return sle_team_send_encoded_packet(node, dst_id, &app_packet);
}
static int sle_team_forward_packet(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    sle_team_app_packet_t forwarded;
    uint16_t incoming_term;
    uint8_t stale_child_hello = 0U;
    if (node == NULL || app == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER ||
        node->cfg.relay_enabled == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    incoming_term = app->leader_term != 0U ? app->leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    if (app->app_msg_type == SLE_TEAM_APP_HELLO && app->src_id != node->cfg.leader_id &&
        app->dst_id != node->cfg.leader_id && incoming_term < node->cfg.leader_term) {
        stale_child_hello = 1U;
    }
    /* Relay members only forward leader<->child traffic. The stale child HELLO
     * exception lets an old child reach the current leader for a fresh policy. */
    if (!((app->src_id == node->cfg.leader_id && app->dst_id != node->cfg.leader_id) ||
        (app->src_id != node->cfg.leader_id && app->dst_id == node->cfg.leader_id) ||
        stale_child_hello != 0U)) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (app->ttl <= 1U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    forwarded = *app;
    if (stale_child_hello != 0U) {
        forwarded.dst_id = node->cfg.leader_id;
    }
    forwarded.ttl = (uint8_t)(app->ttl - 1U);
    return sle_team_send_encoded_packet(node, forwarded.dst_id, &forwarded);
}
static void sle_team_note_leader_packet(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* Any valid leader-origin downlink proves leader/parent liveness to a relay. */
    uint32_t now_s;
    if (node == NULL || app == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER ||
        app->src_id != node->cfg.leader_id) {
        return;
    }
    now_s = sle_team_now(node);
    node->last_leader_seen_s = now_s;
    node->last_parent_seen_s = now_s;
}
static uint8_t sle_team_msg_can_migrate_leader(uint8_t msg_type)
{
    return (uint8_t)(msg_type == SLE_TEAM_APP_ROUTE_UPDATE);
}

/* Clear member-local policy before accepting a newer authority/term. */
static void sle_team_reset_member_policy_state(sle_team_node_t *node)
{
    if (node == NULL) { return; }
    node->joined = 0U;
    node->state = SLE_TEAM_NET_WAIT_POLICY;
    node->last_hello_s = 0U;
    node->last_heartbeat_s = 0U;
    node->last_config_s = 0U;
    node->last_leader_seen_s = 0U;
    node->last_parent_seen_s = 0U;
    node->upstream_parent_id = 0U;
    node->upstream_parent_state = SLE_TEAM_PARENT_WAIT_POLICY;
    node->cfg.relay_allowed = 0U;
    node->cfg.relay_enabled = 0U;
    node->cfg.relay_tier = 0U;
    node->cfg.max_downstream = 0U;
    node->cfg.relay_discovery_only = 0U;
}
static int sle_team_accept_leader_term(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    uint16_t incoming_term;
    if (node == NULL || app == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return SLE_TEAM_OK;
    }
    incoming_term = app->leader_term != 0U ? app->leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    /* Terms are not leader election here; they are a safety fence so stale
     * packets from an old authority cannot overwrite the current policy. */
    if (app->src_id != node->cfg.leader_id) {
        if (sle_team_valid_member_id(app->src_id) != 0U &&
            (app->dst_id == node->cfg.self_id || app->dst_id == SLE_TEAM_BROADCAST_ID) &&
            incoming_term > node->cfg.leader_term &&
            sle_team_msg_can_migrate_leader(app->app_msg_type) != 0U) {
            node->cfg.leader_id = app->src_id;
            node->cfg.leader_term = incoming_term;
            sle_team_reset_member_policy_state(node);
        }
        return SLE_TEAM_OK;
    }
    if (node->cfg.leader_term == 0U) {
        node->cfg.leader_term = incoming_term;
    }
    if (incoming_term < node->cfg.leader_term) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (incoming_term > node->cfg.leader_term) {
        node->cfg.leader_term = incoming_term;
    }
    return SLE_TEAM_OK;
}
static uint8_t sle_team_leader_accepts_stale_hello(const sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    uint16_t incoming_term;
    if (node == NULL || app == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER ||
        app->app_msg_type != SLE_TEAM_APP_HELLO) {
        return 0U;
    }
    incoming_term = app->leader_term != 0U ? app->leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    return (uint8_t)(node->cfg.leader_term > incoming_term ? 1U : 0U);
}

/* Children of a lost relay become recovery candidates and wait for fresh policy. */
static void sle_team_mark_relay_children_offline(sle_team_node_t *node, uint8_t relay_id)
{
    uint8_t i;
    uint8_t affected_count = 0U;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER || sle_team_valid_member_id(relay_id) == 0U) {
        return;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        sle_team_member_record_t *child = &node->members[i];
        if ((child->online != 0U || child->policy_pending != 0U) &&
            child->parent_id == relay_id && child->member_id != relay_id) {
            uint8_t child_id = child->member_id;
            child->online = 0U;
            sle_team_clear_stale_join_policy(child);
            child->member_id = child_id;
            child->relay_recovery_candidate = 1U;
            affected_count++;
        }
    }
    if (affected_count != 0U) {
        node->relay_recovery_pending = 1U;
        node->relay_recovery_lost_relay_id = relay_id;
        sle_team_choose_relay_recovery_candidate(node);
        sle_team_log(node, "relay recovery pending");
    }
}

/* One member offline path handles both heartbeat timeout and explicit leave. */
static void sle_team_mark_member_offline(sle_team_node_t *node, sle_team_member_record_t *member)
{
    uint8_t was_relay;
    if (node == NULL || member == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER ||
        sle_team_valid_member_id(member->member_id) == 0U ||
        (member->online == 0U && member->policy_pending == 0U)) {
        return;
    }
    if (member->online == 0U) {
        if (node->relay_recovery_selected_id == member->member_id) {
            node->relay_recovery_selected_id = 0U;
        }
        (void)memset(member, 0, sizeof(*member));
        sle_team_recompute_child_counts(node);
        return;
    }
    was_relay = member->relay_allowed;
    member->online = 0U;
    sle_team_clear_stale_join_policy(member);
    if (was_relay != 0U) {
        sle_team_mark_relay_children_offline(node, member->member_id);
        if (node->ops.on_relay_offline != NULL) { node->ops.on_relay_offline(node->ops.user_ctx, member->member_id); }
    }
    sle_team_recompute_child_counts(node);
}
int sle_team_node_allow_all_members(sle_team_node_t *node)
{
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    node->cfg.member_filter_enabled = 0U;
    node->cfg.allowed_member_count = 0U;
    (void)memset(node->cfg.allowed_member_ids, 0, sizeof(node->cfg.allowed_member_ids));
    return SLE_TEAM_OK;
}

/* Replace the allow-list with a de-duplicated set of valid route ids. */
int sle_team_node_set_allowed_members(sle_team_node_t *node, const uint8_t *member_ids, uint8_t count)
{
    uint8_t i;
    uint8_t unique_count = 0U;
    uint8_t unique_ids[SLE_TEAM_MAX_MEMBERS];
    if (node == NULL || (count != 0U && member_ids == NULL) || count > SLE_TEAM_MAX_MEMBERS) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset(unique_ids, 0, sizeof(unique_ids));
    for (i = 0U; i < count; i++) {
        if (sle_team_valid_member_id(member_ids[i]) == 0U) {
            return SLE_TEAM_ERR_ARG;
        }
        if (sle_team_id_in_list(unique_ids, unique_count, member_ids[i]) == 0U) {
            unique_ids[unique_count++] = member_ids[i];
        }
    }
    node->cfg.member_filter_enabled = 1U;
    node->cfg.allowed_member_count = unique_count;
    (void)memset(node->cfg.allowed_member_ids, 0, sizeof(node->cfg.allowed_member_ids));
    if (unique_count != 0U) {
        (void)memcpy(node->cfg.allowed_member_ids, unique_ids, unique_count);
    }
    return SLE_TEAM_OK;
}
int sle_team_node_add_allowed_member(sle_team_node_t *node, uint8_t member_id)
{
    if (node == NULL || sle_team_valid_member_id(member_id) == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    if (node->cfg.member_filter_enabled == 0U) {
        node->cfg.member_filter_enabled = 1U;
        node->cfg.allowed_member_count = 0U;
    }
    if (sle_team_id_in_list(node->cfg.allowed_member_ids, node->cfg.allowed_member_count, member_id) != 0U) {
        return SLE_TEAM_OK;
    }
    if (node->cfg.allowed_member_count >= SLE_TEAM_MAX_MEMBERS) {
        return SLE_TEAM_ERR_BUF;
    }
    node->cfg.allowed_member_ids[node->cfg.allowed_member_count++] = member_id;
    return SLE_TEAM_OK;
}
int sle_team_node_remove_allowed_member(sle_team_node_t *node, uint8_t member_id)
{
    uint8_t i;
    uint8_t j;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    for (i = 0U; i < node->cfg.allowed_member_count; i++) {
        if (node->cfg.allowed_member_ids[i] != member_id) {
            continue;
        }
        for (j = i; (uint8_t)(j + 1U) < node->cfg.allowed_member_count; j++) {
            node->cfg.allowed_member_ids[j] = node->cfg.allowed_member_ids[j + 1U];
        }
        node->cfg.allowed_member_count--;
        node->cfg.allowed_member_ids[node->cfg.allowed_member_count] = 0U;
        break;
    }
    return SLE_TEAM_OK;
}
int sle_team_node_pairing_start(sle_team_node_t *node)
{
    /* Pairing starts from a closed allow-list and records new HELLOs as pending. */
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return SLE_TEAM_ERR_ARG;
    }
    node->cfg.pairing_enabled = 1U;
    node->cfg.member_filter_enabled = 1U;
    node->cfg.allowed_member_count = 0U;
    (void)memset(node->cfg.allowed_member_ids, 0, sizeof(node->cfg.allowed_member_ids));
    (void)memset(node->pending_members, 0, sizeof(node->pending_members));
    sle_team_log(node, "pairing started");
    return SLE_TEAM_OK;
}
int sle_team_node_pairing_stop(sle_team_node_t *node)
{
    /* Stop is permissive: pending members are allowed, then filtering is opened. */
    uint8_t i;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return SLE_TEAM_ERR_ARG;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->pending_members[i].active != 0U) {
            (void)sle_team_node_add_allowed_member(node, node->pending_members[i].member_id);
            (void)memset(&node->pending_members[i], 0, sizeof(node->pending_members[i]));
        }
    }
    node->cfg.pairing_enabled = 0U;
    node->cfg.member_filter_enabled = 0U;
    sle_team_log(node, "pairing stopped");
    return SLE_TEAM_OK;
}
int sle_team_node_pairing_approve(sle_team_node_t *node, uint8_t member_id)
{
    return sle_team_node_pairing_approve_with_relay(node, member_id, 0U);
}
int sle_team_node_pairing_approve_with_relay(sle_team_node_t *node, uint8_t member_id, uint8_t relay_allowed)
{
    /* Approval creates/updates a member record and immediately pushes policy if possible. */
    sle_team_member_record_t *member;
    int ret;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER || sle_team_valid_member_id(member_id) == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    ret = sle_team_node_add_allowed_member(node, member_id);
    if (ret != SLE_TEAM_OK) {
        return ret;
    }
    member = sle_team_member_slot(node, member_id, 1U);
    if (member == NULL) {
        return SLE_TEAM_ERR_BUF;
    }
    member->online = 0U;
    member->policy_pending = 0U;
    member->relay_allowed = relay_allowed != 0U ? 1U : member->relay_allowed;
    if (member->relay_allowed != 0U) {
        member->relay_tier = 1U;
        member->max_downstream = SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
    }
    (void)sle_team_assign_parent(node, member);
    if (member->parent_id == 0U) {
        return SLE_TEAM_OK;
    }
    member->policy_pending = 1U;
    member->pending_ack_seq = node->next_seq;
    sle_team_recompute_child_counts(node);
    (void)sle_team_node_send_route_update(node, member_id, member->parent_id,
        (uint8_t)SLE_TEAM_PARENT_CONNECTED, member->next_hop_id);
    (void)sle_team_node_send_config(node, member_id);
    (void)sle_team_node_send_ack(node, member_id, 0U, SLE_TEAM_APP_HELLO, 0U);
    sle_team_clear_pending(node, member_id);
    return SLE_TEAM_OK;
}
int sle_team_node_member_select_leader_term(sle_team_node_t *node, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_term)
{
    /* Member-side join resets local policy and starts HELLO retries toward leader. */
    uint16_t next_term;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER || team_id == 0U ||
        team_id == SLE_TEAM_BROADCAST_ID || sle_team_valid_member_id(leader_id) == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    next_term = leader_term != 0U ? leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    if (next_term < node->cfg.leader_term) { return SLE_TEAM_ERR_UNSUPPORTED; }
    node->cfg.team_id = team_id;
    node->cfg.leader_id = leader_id;
    node->cfg.leader_term = next_term;
    node->cfg.channel_hash = channel_hash;
    node->joined = 0U; node->state = SLE_TEAM_NET_WAIT_POLICY;
    node->last_hello_s = 0U; node->last_heartbeat_s = 0U; node->last_config_s = 0U;
    node->last_leader_seen_s = 0U; node->last_parent_seen_s = 0U;
    node->upstream_parent_id = 0U;
    node->upstream_parent_state = SLE_TEAM_PARENT_WAIT_POLICY;
    node->cfg.relay_allowed = 0U; node->cfg.relay_enabled = 0U; node->cfg.relay_tier = 0U;
    node->cfg.max_downstream = 0U; node->cfg.relay_discovery_only = 0U;
    return sle_team_node_send_hello(node, leader_id);
}
int sle_team_node_member_select_leader(sle_team_node_t *node, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash)
{
    uint16_t next_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    if (node != NULL && node->cfg.leader_term != 0U) { next_term = node->cfg.leader_term; }
    return sle_team_node_member_select_leader_term(node, team_id, leader_id, channel_hash, next_term);
}
int sle_team_node_member_leave(sle_team_node_t *node)
{
    /* Explicit leave is best-effort: notify leader, then go fully idle locally. */
    sle_team_alert_body_t alert;
    uint8_t leader_id;
    int ret = SLE_TEAM_OK;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return SLE_TEAM_ERR_ARG;
    }
    leader_id = node->cfg.leader_id;
    if (sle_team_valid_member_id(leader_id) != 0U) {
        (void)memset(&alert, 0, sizeof(alert));
        alert.lost_member_id = node->cfg.self_id;
        alert.reason = SLE_TEAM_ALERT_LEAVE;
        ret = sle_team_node_send_alert(node, leader_id, &alert);
    }
    node->joined = 0U;
    node->state = SLE_TEAM_NET_IDLE;
    node->cfg.leader_id = 0U;
    node->cfg.leader_term = 0U;
    node->last_hello_s = 0U;
    node->last_heartbeat_s = 0U;
    node->last_config_s = 0U;
    node->last_leader_seen_s = 0U;
    node->last_parent_seen_s = 0U;
    node->upstream_parent_id = 0U;
    node->upstream_parent_state = SLE_TEAM_PARENT_IDLE;
    node->cfg.relay_enabled = 0U;
    node->cfg.relay_allowed = 0U;
    return ret == SLE_TEAM_OK ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED;
}
int sle_team_node_member_link_lost(sle_team_node_t *node)
{
    /* Silent parent loss keeps the leader target and resumes HELLO/policy wait. */
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return SLE_TEAM_ERR_ARG;
    }
    node->joined = 0U;
    node->state = SLE_TEAM_NET_WAIT_POLICY;
    node->last_hello_s = 0U;
    node->last_heartbeat_s = 0U;
    node->last_config_s = 0U;
    node->last_leader_seen_s = 0U;
    node->last_parent_seen_s = 0U;
    node->upstream_parent_id = 0U;
    node->upstream_parent_state = SLE_TEAM_PARENT_WAIT_POLICY;
    node->cfg.relay_enabled = 0U;
    sle_team_log(node, "parent lost, waiting for leader policy");
    return SLE_TEAM_OK;
}
int sle_team_node_member_offline(sle_team_node_t *node, uint8_t member_id)
{
    /* External transport tells the leader a direct member disappeared. */
    sle_team_member_record_t *member;
    if (node == NULL || node->cfg.role != SLE_TEAM_ROLE_LEADER || sle_team_valid_member_id(member_id) == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    member = sle_team_find_member(node, member_id);
    if (member == NULL) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (member->parent_id != node->cfg.self_id && member->parent_id != node->cfg.leader_id) {
        return SLE_TEAM_OK;
    }
    sle_team_mark_member_offline(node, member);
    return SLE_TEAM_OK;
}
static int sle_team_handle_hello(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /*
     * Leader HELLO handling:
     *   1. enforce firmware/allow-list;
     *   2. record pending members during pairing;
     *   3. keep proven policy when safe;
     *   4. otherwise assign parent and send fresh policy.
     */
    sle_team_hello_body_t hello;
    sle_team_member_record_t *member;
    sle_team_member_record_t old_policy;
    sle_team_pending_member_t *pending;
    uint8_t keep_existing_policy;
    uint8_t preserved_next_hop = 0U;
    uint16_t hello_fw_compat;
    int ret;
    if (node == NULL || app == NULL || app->body_len < sizeof(hello)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return SLE_TEAM_OK;
    }
    (void)memcpy(&hello, app->body, sizeof(hello));
    hello_fw_compat = (uint16_t)(((uint16_t)hello.fw_compat_hi << 8U) | hello.fw_compat_lo);
    /* Firmware compatibility is enforced at HELLO as well as scan time. This
     * protects the group if an old image slips past advertising filters. */
    if (node->cfg.fw_compat != SLE_TEAM_FW_COMPAT_ANY && hello_fw_compat != node->cfg.fw_compat) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (sle_team_node_is_member_allowed(node, app->src_id) == 0U) {
        if (node->cfg.pairing_enabled != 0U) {
            pending = sle_team_pending_slot(node, app->src_id, 1U);
            if (pending == NULL) {
                return SLE_TEAM_ERR_BUF;
            }
            pending->role = hello.role;
            pending->battery_percent = hello.battery_percent;
            pending->mac_ready = hello.mac_ready;
            (void)memcpy(pending->mac, hello.mac, sizeof(pending->mac));
            pending->last_seen_s = sle_team_now(node);
            return SLE_TEAM_OK;
        }
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    member = sle_team_member_slot(node, app->src_id, 1U);
    if (member == NULL) { return SLE_TEAM_ERR_BUF; }
    member->role = hello.role;
    member->battery_percent = hello.battery_percent;
    member->mac_ready = hello.mac_ready;
    (void)memcpy(member->mac, hello.mac, sizeof(member->mac));
    member->last_seen_s = sle_team_now(node);
    member->last_seq = app->seq;
    member->last_rssi_dbm = sle_team_rssi_dbm(node);
    keep_existing_policy = (uint8_t)(member->parent_id != 0U &&
        member->online != 0U &&
        node->relay_recovery_lost_relay_id != member->member_id &&
        member->relay_recovery_candidate == 0U);
    if (keep_existing_policy != 0U) {
        return sle_team_resend_member_policy(node, member, app->seq);
    }
    if (member->policy_pending != 0U &&
        member->next_hop_id != 0U &&
        member->next_hop_id != node->cfg.self_id &&
        member->next_hop_id != node->cfg.leader_id) {
        preserved_next_hop = member->next_hop_id;
    }
    old_policy = *member;
    member->online = 0U;
    sle_team_clear_stale_join_policy(member);
    if (sle_team_assign_parent(node, member) == 0U) { return SLE_TEAM_OK; }
    /* If the HELLO came through a relay and the member would otherwise be
     * leader-direct, prefer that ingress relay as the real parent. */
    if (member->parent_id == node->cfg.leader_id &&
        sle_team_ingress_relay_can_parent(node, node->rx_ingress_relay_id, member->member_id) != 0U) {
        member->parent_id = node->rx_ingress_relay_id;
        member->next_hop_id = node->rx_ingress_relay_id;
        sle_team_recompute_child_counts(node);
    }
    if (preserved_next_hop != 0U) {
        member->next_hop_id = preserved_next_hop;
    }
    member->policy_pending = 1U;
    ret = sle_team_resend_member_policy(node, member, app->seq);
    if (ret != SLE_TEAM_OK && old_policy.policy_pending != 0U) { *member = old_policy; }
    return ret;
}
static int sle_team_handle_ack(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* ACK clears pending policy only when it matches the currently expected seq. */
    sle_team_ack_body_t ack;
    if (node == NULL || app == NULL || app->body_len < sizeof(ack)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    (void)memcpy(&ack, app->body, sizeof(ack));
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER && ack.status_code == 0U) {
        sle_team_member_record_t *member = sle_team_find_member(node, app->src_id);
        if (member != NULL && member->policy_pending != 0U &&
            ack.ack_seq == member->pending_ack_seq &&
            (ack.acked_msg_type == SLE_TEAM_APP_ROUTE_UPDATE || ack.acked_msg_type == SLE_TEAM_APP_CONFIG ||
            ack.acked_msg_type == SLE_TEAM_APP_HELLO)) {
            return sle_team_confirm_member_online(node, member);
        }
        if (member != NULL && member->policy_pending != 0U) {
            return SLE_TEAM_ERR_UNSUPPORTED;
        }
    }
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER && app->src_id == node->cfg.leader_id &&
        ack.acked_msg_type == SLE_TEAM_APP_HELLO && ack.status_code == 0U) {
        node->joined = 1U;
        node->state = SLE_TEAM_NET_ONLINE;
        node->last_leader_seen_s = sle_team_now(node);
        node->last_parent_seen_s = node->last_leader_seen_s;
        if (node->upstream_parent_id == 0U) {
            node->upstream_parent_id = node->cfg.leader_id;
        }
        node->upstream_parent_state = SLE_TEAM_PARENT_CONNECTED;
        node->cfg.relay_enabled = node->cfg.relay_allowed;
        sle_team_mark_joined(node, node->cfg.self_id);
    }
    return SLE_TEAM_OK;
}
static int sle_team_handle_config(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* Members apply leader timing and relay permission here; parent is separate. */
    sle_team_config_body_t cfg_body;
    if (node == NULL || app == NULL || app->body_len < sizeof(cfg_body)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (node->cfg.role != SLE_TEAM_ROLE_MEMBER || app->src_id != node->cfg.leader_id) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    (void)memcpy(&cfg_body, app->body, sizeof(cfg_body));
    node->cfg.report_interval_s = cfg_body.report_interval_s;
    node->cfg.warn_distance_m = cfg_body.warn_distance_m;
    node->cfg.lost_distance_m = cfg_body.lost_distance_m;
    node->cfg.heartbeat_timeout_s = cfg_body.heartbeat_timeout_s;
    node->cfg.relay_allowed = cfg_body.relay_allowed != 0U ? 1U : 0U;
    node->cfg.relay_tier = node->cfg.relay_allowed != 0U ? cfg_body.relay_tier : 0U;
    node->cfg.max_downstream = node->cfg.relay_allowed != 0U ? cfg_body.max_downstream : 0U;
    node->cfg.relay_discovery_only = 0U;
    node->cfg.relay_enabled = (node->joined != 0U && node->cfg.relay_allowed != 0U) ? 1U : 0U;
    node->last_config_s = sle_team_now(node);
    node->last_leader_seen_s = node->last_config_s;
    return SLE_TEAM_OK;
}
static int sle_team_handle_route_update(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* ROUTE_UPDATE is the member's authoritative parent assignment. */
    sle_team_route_update_body_t route_update;
    uint8_t notify_joined;
    uint32_t now_s;
    if (node == NULL || app == NULL || app->body_len < sizeof(route_update)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (node->cfg.role != SLE_TEAM_ROLE_MEMBER || app->src_id != node->cfg.leader_id) {
        return SLE_TEAM_OK;
    }
    (void)memcpy(&route_update, app->body, sizeof(route_update));
    if (sle_team_valid_member_id(route_update.parent_id) == 0U) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (node->upstream_parent_id != 0U && node->upstream_parent_id != route_update.parent_id) {
        /* Parent change is a real topology change. Mark not joined until the
         * new parent path proves itself with ACK/heartbeat traffic. */
        node->joined = 0U;
        node->state = SLE_TEAM_NET_WAIT_POLICY;
        node->last_hello_s = 0U;
        node->last_heartbeat_s = 0U;
    }
    node->upstream_parent_id = route_update.parent_id;
    node->upstream_parent_state = (sle_team_parent_state_t)route_update.parent_state;
    node->last_leader_seen_s = sle_team_now(node);
    if ((route_update.reserved & SLE_TEAM_ROUTE_UPDATE_FLAG_RELAY_GRANT) != 0U) {
        node->cfg.relay_allowed = 1U;
        node->cfg.relay_enabled = node->joined != 0U ? 1U : 0U;
        if (node->cfg.max_downstream == 0U) {
            node->cfg.max_downstream = SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
        }
    } else {
        node->cfg.relay_allowed = 0U;
        node->cfg.relay_enabled = 0U;
        node->cfg.relay_tier = 0U;
        node->cfg.max_downstream = 0U;
    }
    if (route_update.parent_state == (uint8_t)SLE_TEAM_PARENT_CONNECTED) {
        notify_joined = node->joined == 0U ? 1U : 0U;
        now_s = sle_team_now(node);
        node->joined = 1U;
        node->state = SLE_TEAM_NET_ONLINE;
        node->last_leader_seen_s = now_s;
        node->last_parent_seen_s = now_s;
        node->cfg.relay_enabled = node->cfg.relay_allowed;
        if (notify_joined != 0U) {
            sle_team_mark_joined(node, node->cfg.self_id);
        }
        (void)sle_team_node_send_ack(node, node->cfg.leader_id, app->seq, SLE_TEAM_APP_ROUTE_UPDATE, 0U);
    }
    return SLE_TEAM_OK;
}
static int sle_team_handle_heartbeat(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* Heartbeats are liveness evidence and can also confirm a pending policy. */
    sle_team_heartbeat_body_t hb;
    sle_team_member_record_t *member;
    if (node == NULL || app == NULL || app->body_len < sizeof(hb)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    (void)memcpy(&hb, app->body, sizeof(hb));
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER) {
        if (app->src_id == node->cfg.leader_id) {
            node->last_leader_seen_s = sle_team_now(node);
        }
        return SLE_TEAM_OK;
    }
    if (sle_team_node_is_member_allowed(node, app->src_id) == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    member = sle_team_find_member(node, app->src_id);
    if (member == NULL) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    member->battery_percent = hb.battery_percent;
    member->fix_status = hb.fix_status;
    member->last_rssi_dbm = hb.rssi_dbm;
    member->last_seen_s = sle_team_now(node);
    member->last_seq = app->seq;
    if (sle_team_recovery_policy_required(node, member) != 0U) {
        return sle_team_refresh_recovery_policy(node, member);
    }
    if (member->parent_id == 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    if (member->policy_pending != 0U &&
        sle_team_pending_live_packet_confirms(node, member) == 0U) {
        return SLE_TEAM_OK;
    }
    return sle_team_confirm_member_online(node, member);
}
static int sle_team_handle_position(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* Leader keeps last-known member position even if the member later drops. */
    sle_team_pos_body_t pos;
    sle_team_member_record_t *member;
    if (node == NULL || app == NULL || app->body_len < sizeof(pos)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    (void)memcpy(&pos, app->body, sizeof(pos));
    if (node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return SLE_TEAM_OK;
    }
    member = sle_team_find_member(node, app->src_id);
    if (member == NULL || (member->online == 0U && member->policy_pending == 0U)) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    member->battery_percent = pos.battery_percent;
    member->fix_status = pos.fix_status;
    if (pos.fix_status != 0U) {
        member->position_valid = 1U;
        member->latitude_e6 = pos.latitude_e6;
        member->longitude_e6 = pos.longitude_e6;
        member->speed_cms = pos.speed_cms;
        member->heading_deg = pos.heading_deg;
    }
    member->sat_count = pos.sat_count;
    member->last_seen_s = sle_team_now(node);
    member->last_seq = app->seq;
    if (member->policy_pending != 0U) {
        if (sle_team_pending_live_packet_confirms(node, member) == 0U) {
            return SLE_TEAM_OK;
        }
        return sle_team_confirm_member_online(node, member);
    }
    if (node->ops.on_position != NULL) {
        node->ops.on_position(node->ops.user_ctx, app->src_id, &pos);
    }
    return SLE_TEAM_OK;
}
static int sle_team_handle_alert(sle_team_node_t *node, const sle_team_app_packet_t *app)
{
    /* Leave alerts reuse the same offline path as timeout so relay children recover. */
    sle_team_alert_body_t alert;
    sle_team_member_record_t *member;
    if (node == NULL || app == NULL || app->body_len < sizeof(alert)) {
        return SLE_TEAM_ERR_FORMAT;
    }
    (void)memcpy(&alert, app->body, sizeof(alert));
    if (node->ops.on_alert != NULL) {
        node->ops.on_alert(node->ops.user_ctx, alert.lost_member_id, alert.reason);
    }
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER && alert.reason == SLE_TEAM_ALERT_LEAVE) {
        member = sle_team_find_member(node, alert.lost_member_id);
        if (member != NULL) {
            sle_team_mark_member_offline(node, member);
        }
    }
    return SLE_TEAM_OK;
}
int sle_team_node_init(sle_team_node_t *node, const sle_team_node_cfg_t *cfg, const sle_team_node_ops_t *ops)
{
    /* Initialize one logical node instance with its fixed identity and runtime callbacks. */
    if (node == NULL || cfg == NULL || ops == NULL || ops->send == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset(node, 0, sizeof(*node));
    node->cfg = *cfg;
    node->ops = *ops;
    node->next_seq = 1U;
    if (node->cfg.leader_term == 0U) {
        node->cfg.leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    }
    if (node->cfg.default_ttl == 0U) {
        node->cfg.default_ttl = 4U;
    }
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        node->state = SLE_TEAM_NET_ONLINE;
        node->joined = 1U;
        node->upstream_parent_state = SLE_TEAM_PARENT_IDLE;
        if (node->cfg.max_downstream == 0U) {
            node->cfg.max_downstream = SLE_TEAM_DIRECT_CAP_DEFAULT;
        }
    } else {
        node->state = SLE_TEAM_NET_WAIT_POLICY;
        node->joined = 0U;
        node->upstream_parent_state = SLE_TEAM_PARENT_WAIT_POLICY;
        node->cfg.relay_allowed = 0U;
        node->cfg.relay_enabled = 0U;
        node->cfg.relay_tier = 0U;
        node->cfg.max_downstream = 0U;
    }
    return SLE_TEAM_OK;
}
void sle_team_node_tick(sle_team_node_t *node)
{
    uint8_t i;
    uint8_t leader_heartbeat_due;
    uint8_t leader_heartbeat_sent = 0U;
    uint32_t now_s;
    if (node == NULL) {
        return;
    }
    now_s = sle_team_now(node);
    /* tick() is the portable scheduler: leaders time out stale records and
     * send keepalives; members send HELLO until policy arrives, then heartbeat. */
    leader_heartbeat_due = (uint8_t)(node->cfg.heartbeat_interval_s != 0U &&
        (node->last_heartbeat_s == 0U ||
        (uint32_t)(now_s - node->last_heartbeat_s) >= node->cfg.heartbeat_interval_s));
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        if (node->cfg.heartbeat_timeout_s == 0U) {
            return;
        }
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            sle_team_member_record_t *member = &node->members[i];
            if (member->policy_pending != 0U && member->last_seen_s != 0U &&
                (uint32_t)(now_s - member->last_seen_s) > node->cfg.heartbeat_timeout_s) {
                sle_team_mark_member_offline(node, member);
                continue;
            }
            if (member->online == 0U || member->last_seen_s == 0U) {
                continue;
            }
            if ((uint32_t)(now_s - member->last_seen_s) > node->cfg.heartbeat_timeout_s) {
                sle_team_mark_member_offline(node, member);
                continue;
            }
            if (leader_heartbeat_due != 0U &&
                (uint32_t)(now_s - member->last_seen_s) <= node->cfg.heartbeat_timeout_s &&
                sle_team_valid_member_id(member->member_id) != 0U) {
                (void)sle_team_send_leader_heartbeat(node, member->member_id);
                leader_heartbeat_sent = 1U;
            }
        }
        if (leader_heartbeat_sent != 0U) {
            node->last_heartbeat_s = now_s;
        }
        sle_team_recompute_child_counts(node);
        return;
    }
    if (node->joined != 0U && node->cfg.heartbeat_timeout_s != 0U && node->last_leader_seen_s != 0U &&
        (uint32_t)(now_s - node->last_leader_seen_s) > node->cfg.heartbeat_timeout_s) {
        (void)sle_team_node_member_link_lost(node);
    }
    if (node->joined == 0U && node->state != SLE_TEAM_NET_IDLE && sle_team_valid_member_id(node->cfg.leader_id) != 0U) {
        if (node->last_hello_s == 0U || (uint32_t)(now_s - node->last_hello_s) >= SLE_TEAM_MEMBER_HELLO_INTERVAL_S) {
            (void)sle_team_node_send_hello(node, node->cfg.leader_id);
            node->last_hello_s = now_s;
            node->state = SLE_TEAM_NET_JOINING;
        }
        return;
    }
    if (node->joined != 0U && node->cfg.heartbeat_interval_s != 0U &&
        (node->last_heartbeat_s == 0U || (uint32_t)(now_s - node->last_heartbeat_s) >= node->cfg.heartbeat_interval_s)) {
        (void)sle_team_node_send_heartbeat(node, node->cfg.leader_id,
            sle_team_battery_percent(node), sle_team_rssi_dbm(node), 1U);
        node->last_heartbeat_s = now_s;
    }
}
int sle_team_node_on_packet(sle_team_node_t *node, const uint8_t *buf, size_t buf_len)
{
    /* Ingress path: unwrap transport, validate authority, then dispatch by app message type. */
    sle_team_mesh_packet_t mesh_packet;
    sle_team_app_packet_t app_packet;
    const uint8_t *app_payload = NULL;
    uint16_t app_payload_len = 0U;
    uint8_t channel_hash = 0U;
    uint8_t cipher_mac[2];
    int ret;
    if (node == NULL || buf == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    ret = sle_team_decode_mesh_packet(&mesh_packet, buf, buf_len);
    if (ret != SLE_TEAM_OK) {
        return ret;
    }
    ret = sle_team_unwrap_mesh_group_data(&mesh_packet, &channel_hash, cipher_mac, &app_payload, &app_payload_len);
    if (ret != SLE_TEAM_OK || channel_hash != node->cfg.channel_hash) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    ret = sle_team_decode_app_packet(&app_packet, app_payload, app_payload_len);
    if (ret != SLE_TEAM_OK) {
        return ret;
    }
    if (app_packet.team_id != node->cfg.team_id) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    /* Decode order matters: reject wrong team/term before forwarding or
     * applying state changes, then let members relay packets not addressed to them. */
    ret = sle_team_accept_leader_term(node, &app_packet);
    if (ret != SLE_TEAM_OK) {
        return ret;
    }
    sle_team_note_leader_packet(node, &app_packet);
    if (app_packet.dst_id != node->cfg.self_id && app_packet.dst_id != SLE_TEAM_BROADCAST_ID &&
        sle_team_leader_accepts_stale_hello(node, &app_packet) == 0U) {
        return sle_team_forward_packet(node, &app_packet);
    }
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER && app_packet.src_id != node->cfg.leader_id &&
        app_packet.dst_id != SLE_TEAM_BROADCAST_ID) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    switch (app_packet.app_msg_type) {
        case SLE_TEAM_APP_HELLO:
            return sle_team_handle_hello(node, &app_packet);
        case SLE_TEAM_APP_ACK:
            return sle_team_handle_ack(node, &app_packet);
        case SLE_TEAM_APP_CONFIG:
            return sle_team_handle_config(node, &app_packet);
        case SLE_TEAM_APP_ROUTE_UPDATE:
            return sle_team_handle_route_update(node, &app_packet);
        case SLE_TEAM_APP_HEARTBEAT:
            return sle_team_handle_heartbeat(node, &app_packet);
        case SLE_TEAM_APP_POS_REPORT:
            return sle_team_handle_position(node, &app_packet);
        case SLE_TEAM_APP_ALERT:
            return sle_team_handle_alert(node, &app_packet);
        default:
            return SLE_TEAM_ERR_UNSUPPORTED;
    }
}
int sle_team_node_send_hello(sle_team_node_t *node, uint8_t dst_id)
{
    /* HELLO advertises identity plus firmware compatibility. */
    sle_team_hello_body_t hello;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset(&hello, 0, sizeof(hello));
    hello.device_id = node->cfg.self_id;
    hello.role = (uint8_t)node->cfg.role;
    hello.battery_percent = sle_team_battery_percent(node);
    hello.mac_ready = node->cfg.self_mac_ready;
    hello.fw_compat_lo = (uint8_t)(node->cfg.fw_compat & 0xFFU);
    hello.fw_compat_hi = (uint8_t)((node->cfg.fw_compat >> 8U) & 0xFFU);
    (void)memcpy(hello.mac, node->cfg.self_mac, sizeof(hello.mac));
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_HELLO, (const uint8_t *)&hello, sizeof(hello));
}
int sle_team_node_send_heartbeat(sle_team_node_t *node, uint8_t dst_id, uint8_t battery_percent,
    int8_t rssi_dbm, uint8_t fix_status)
{
    /* Heartbeat carries liveness, battery, and a tiny telemetry sample. */
    sle_team_heartbeat_body_t hb;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    hb.battery_percent = battery_percent;
    hb.rssi_dbm = rssi_dbm;
    hb.fix_status = fix_status;
    hb.reserved = 0U;
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_HEARTBEAT, (const uint8_t *)&hb, sizeof(hb));
}
static int sle_team_send_leader_heartbeat(sle_team_node_t *node, uint8_t member_id)
{
    return sle_team_node_send_heartbeat(node, member_id, 100U, SLE_TEAM_RSSI_UNKNOWN, 1U);
}
int sle_team_node_send_position(sle_team_node_t *node, uint8_t dst_id, const sle_team_pos_body_t *pos)
{
    /* Position is raw telemetry, usually from GPS or a fallback source. */
    if (node == NULL || pos == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_POS_REPORT, (const uint8_t *)pos, sizeof(*pos));
}
int sle_team_node_send_alert(sle_team_node_t *node, uint8_t dst_id, const sle_team_alert_body_t *alert)
{
    /* Alert is used for leave/lost notifications and last-known position handoff. */
    if (node == NULL || alert == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_ALERT, (const uint8_t *)alert, sizeof(*alert));
}
int sle_team_node_send_config(sle_team_node_t *node, uint8_t dst_id)
{
    /* CONFIG carries timing and relay permission knobs for the chosen parent. */
    sle_team_config_body_t cfg_body;
    const sle_team_member_record_t *member;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset(&cfg_body, 0, sizeof(cfg_body));
    cfg_body.report_interval_s = node->cfg.report_interval_s;
    cfg_body.warn_distance_m = node->cfg.warn_distance_m;
    cfg_body.lost_distance_m = node->cfg.lost_distance_m;
    cfg_body.heartbeat_timeout_s = node->cfg.heartbeat_timeout_s;
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        member = sle_team_find_member_const(node, dst_id);
        if (member != NULL && member->relay_allowed != 0U) {
            cfg_body.relay_allowed = 1U;
            cfg_body.relay_tier = member->relay_tier != 0U ? member->relay_tier : 1U;
            cfg_body.max_downstream = member->max_downstream != 0U ?
                member->max_downstream : SLE_TEAM_RELAY_CHILD_CAP_DEFAULT;
        }
    } else {
        cfg_body.relay_allowed = node->cfg.relay_allowed;
        cfg_body.relay_tier = node->cfg.relay_tier;
        cfg_body.max_downstream = sle_team_relay_child_cap(node);
    }
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_CONFIG, (const uint8_t *)&cfg_body, sizeof(cfg_body));
}
int sle_team_node_send_ack(sle_team_node_t *node, uint8_t dst_id, uint16_t ack_seq, uint8_t acked_msg_type,
    uint8_t status_code)
{
    /* ACK tags the policy or hello sequence that the receiver should consider settled. */
    sle_team_ack_body_t ack;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    ack.ack_seq = ack_seq;
    ack.acked_msg_type = acked_msg_type;
    ack.status_code = status_code;
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_ACK, (const uint8_t *)&ack, sizeof(ack));
}
int sle_team_node_send_route_update(sle_team_node_t *node, uint8_t dst_id, uint8_t parent_id,
    uint8_t parent_state, uint8_t next_hop_id)
{
    /* ROUTE_UPDATE says who the parent is and whether the path is ready. */
    sle_team_route_update_body_t update;
    const sle_team_member_record_t *member;
    if (node == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    update.parent_id = parent_id;
    update.next_hop_id = next_hop_id != 0U ? next_hop_id : parent_id;
    update.parent_state = parent_state;
    update.reserved = 0U;
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        member = sle_team_find_member_const(node, dst_id);
        if (member != NULL && member->relay_allowed != 0U) {
            update.reserved |= SLE_TEAM_ROUTE_UPDATE_FLAG_RELAY_GRANT;
        }
    }
    return sle_team_build_and_send(node, dst_id, SLE_TEAM_APP_ROUTE_UPDATE,
        (const uint8_t *)&update, sizeof(update));
}
const sle_team_member_record_t *sle_team_node_find_member(const sle_team_node_t *node, uint8_t member_id)
{
    return sle_team_find_member_const(node, member_id);
}
