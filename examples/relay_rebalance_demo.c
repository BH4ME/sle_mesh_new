#include "sle_team_node.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define DEMO_AUTO_RELAY_MAX 3U
#define DEMO_RELAY_REVOKE_STALE_FACTOR 2U
#define DEMO_RELAY_CANDIDATE_MIN_RSSI (-92)

typedef struct {
    uint8_t member_id;
    uint8_t online;
    uint8_t relay_allowed;
    int8_t last_rssi_dbm;
    uint32_t last_seen_s;
} demo_member_t;

typedef struct {
    demo_member_t members[SLE_TEAM_MAX_MEMBERS];
    uint8_t relay_online_count;
    uint8_t relay_target_count;
} demo_cluster_t;

static uint8_t demo_elapsed_exceeds(uint32_t now_s, uint32_t mark_s, uint32_t window_s)
{
    return (uint8_t)((uint32_t)(now_s - mark_s) > window_s ? 1U : 0U);
}

static uint8_t demo_relay_target_for_online(uint8_t online_count)
{
    if (online_count == 0U || online_count <= SLE_TEAM_MAX_DIRECT_CONNECTIONS) {
        return 0U;
    }
    if (online_count <= (uint8_t)(SLE_TEAM_MAX_DIRECT_CONNECTIONS * 2U)) {
        return 2U;
    }
    return DEMO_AUTO_RELAY_MAX;
}

static uint8_t demo_member_is_relay_candidate(const demo_member_t *member, uint32_t now_s, uint16_t timeout_s)
{
    if (member == NULL || member->online == 0U) {
        return 0U;
    }
    if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    if (member->relay_allowed != 0U) {
        return 0U;
    }
    if (demo_elapsed_exceeds(now_s, member->last_seen_s, timeout_s) != 0U) {
        return 0U;
    }
    if (member->last_rssi_dbm != SLE_TEAM_RSSI_UNKNOWN &&
        member->last_rssi_dbm < DEMO_RELAY_CANDIDATE_MIN_RSSI) {
        return 0U;
    }
    return 1U;
}

static demo_member_t *demo_pick_best_candidate(demo_cluster_t *cluster, uint32_t now_s, uint16_t timeout_s)
{
    uint8_t i;
    demo_member_t *best = NULL;
    int8_t best_rssi = SLE_TEAM_RSSI_UNKNOWN;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        demo_member_t *member = &cluster->members[i];
        int8_t member_rssi;

        if (demo_member_is_relay_candidate(member, now_s, timeout_s) == 0U) {
            continue;
        }
        /* Unknown RSSI must be treated as weakest so known links are promoted first. */
        member_rssi = (member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN) ? (int8_t)(-128) : member->last_rssi_dbm;
        if (best == NULL || member_rssi > best_rssi) {
            best = member;
            best_rssi = member_rssi;
        }
    }
    return best;
}

static void demo_rebalance_relays(demo_cluster_t *cluster, uint32_t now_s, uint16_t timeout_s)
{
    uint8_t i;
    uint8_t online_count = 0U;
    uint8_t relay_count = 0U;
    uint8_t relay_target;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        demo_member_t *member = &cluster->members[i];

        if (member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID || member->relay_allowed == 0U) {
            continue;
        }
        if (member->online == 0U) {
            member->relay_allowed = 0U;
            continue;
        }
        if (demo_elapsed_exceeds(now_s, member->last_seen_s, (uint32_t)timeout_s * DEMO_RELAY_REVOKE_STALE_FACTOR) !=
            0U) {
            member->relay_allowed = 0U;
        }
    }

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        demo_member_t *member = &cluster->members[i];

        if (member->online == 0U || member->member_id == 0U || member->member_id == SLE_TEAM_BROADCAST_ID) {
            continue;
        }
        online_count++;
        if (member->relay_allowed != 0U) {
            relay_count++;
        }
    }

    relay_target = demo_relay_target_for_online(online_count);
    if (relay_target > DEMO_AUTO_RELAY_MAX) {
        relay_target = DEMO_AUTO_RELAY_MAX;
    }

    while (relay_count < relay_target) {
        demo_member_t *candidate = demo_pick_best_candidate(cluster, now_s, timeout_s);

        if (candidate == NULL) {
            break;
        }
        candidate->relay_allowed = 1U;
        relay_count++;
    }

    cluster->relay_online_count = relay_count;
    cluster->relay_target_count = relay_target;
}

static demo_member_t *demo_find_member(demo_cluster_t *cluster, uint8_t member_id)
{
    uint8_t i;

    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (cluster->members[i].member_id == member_id) {
            return &cluster->members[i];
        }
    }
    return NULL;
}

static void demo_set_member_rssi(demo_cluster_t *cluster, uint8_t member_id, int8_t rssi_dbm)
{
    demo_member_t *member = demo_find_member(cluster, member_id);

    assert(member != NULL);
    member->last_rssi_dbm = rssi_dbm;
}

static void demo_seed_members(demo_cluster_t *cluster)
{
    uint8_t id;

    (void)memset(cluster, 0, sizeof(*cluster));
    assert(SLE_TEAM_MAX_MEMBERS >= 30U);

    for (id = 2U; id <= 31U; id++) {
        demo_member_t *member = &cluster->members[id - 2U];

        member->member_id = id;
        member->online = 1U;
        member->relay_allowed = (id == 2U || id == 3U || id == 4U) ? 1U : 0U;
        member->last_seen_s = 100U;
        member->last_rssi_dbm = (int8_t)(-70 - (int8_t)(id - 2U));
    }

    /* Make member 5 the top fallback candidate once a relay slot becomes available. */
    demo_set_member_rssi(cluster, 5U, -55);
    demo_set_member_rssi(cluster, 6U, -60);
    demo_set_member_rssi(cluster, 7U, -65);
    demo_set_member_rssi(cluster, 3U, -58);
}

int main(void)
{
    demo_cluster_t cluster;
    demo_member_t *relay3;
    demo_member_t *relay5;

    demo_seed_members(&cluster);
    relay3 = demo_find_member(&cluster, 3U);
    relay5 = demo_find_member(&cluster, 5U);
    assert(relay3 != NULL && relay5 != NULL);

    demo_rebalance_relays(&cluster, 100U, 10U);
    assert(cluster.relay_target_count == 3U);
    assert(cluster.relay_online_count == 3U);
    assert(relay3->relay_allowed != 0U);

    /* Relay-3 drops: leader should revoke it and auto-promote another member (member-5). */
    relay3->online = 0U;
    demo_rebalance_relays(&cluster, 101U, 10U);
    assert(cluster.relay_target_count == 3U);
    assert(cluster.relay_online_count == 3U);
    assert(relay3->relay_allowed == 0U);
    assert(relay5->relay_allowed != 0U);

    /* Old relay rejoins: if quota is full, it does NOT automatically stay relay. */
    relay3->online = 1U;
    relay3->last_seen_s = 102U;
    demo_rebalance_relays(&cluster, 102U, 10U);
    assert(cluster.relay_online_count == 3U);
    assert(relay3->relay_allowed == 0U);

    /* If current relays become stale, rejoined relay can be elected again. */
    relay5->last_seen_s = 70U; /* stale at now=102 when timeout*factor=20 */
    demo_rebalance_relays(&cluster, 102U, 10U);
    assert(cluster.relay_online_count == 3U);
    assert(relay3->relay_allowed != 0U);

    printf("[relay-rebalance] pass: drop->promote->rejoin behavior verified\n");
    return 0;
}
