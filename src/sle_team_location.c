#include "sle_team_node.h"

#include <string.h>

/* Location helpers live outside the core file to keep GPS/Web fallback logic small. */
static uint8_t sle_team_location_valid_id(uint8_t member_id)
{
    return (uint8_t)(member_id != 0U && member_id != SLE_TEAM_BROADCAST_ID);
}

/* Use the node clock when available; tests can pass no clock and get 0. */
static uint32_t sle_team_location_now(const sle_team_node_t *node)
{
    if (node == NULL || node->ops.now_s == NULL) {
        return 0U;
    }
    return node->ops.now_s(node->ops.user_ctx);
}

/* Find or create a member table slot that can hold last-known position data. */
static sle_team_member_record_t *sle_team_location_slot(sle_team_node_t *node, uint8_t member_id)
{
    uint8_t i;
    sle_team_member_record_t *free_slot = NULL;

    if (node == NULL || sle_team_location_valid_id(member_id) == 0U) {
        return NULL;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (node->members[i].member_id == member_id) {
            return &node->members[i];
        }
        if (free_slot == NULL && node->members[i].member_id == 0U) {
            free_slot = &node->members[i];
        }
    }
    if (free_slot == NULL) {
        return NULL;
    }
    (void)memset(free_slot, 0, sizeof(*free_slot));
    free_slot->member_id = member_id;
    free_slot->role = SLE_TEAM_ROLE_LEADER;
    free_slot->last_rssi_dbm = SLE_TEAM_RSSI_UNKNOWN;
    return free_slot;
}

int sle_team_node_record_local_position(sle_team_node_t *node, const sle_team_pos_body_t *pos)
{
    sle_team_member_record_t *member;

    if (node == NULL || pos == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    member = sle_team_location_slot(node, node->cfg.self_id);
    if (member == NULL) {
        return SLE_TEAM_ERR_BUF;
    }
    /*
     * The local board can publish its own phone/Wi-Fi/GPS fallback location.
     * Store it in the same table shape as remote member positions so both
     * leader and member AP pages can render one node list without special API
     * routes.
     */
    member->role = (uint8_t)node->cfg.role;
    member->online = (uint8_t)(node->cfg.role == SLE_TEAM_ROLE_LEADER || node->joined != 0U ? 1U : 0U);
    member->battery_percent = pos->battery_percent;
    member->fix_status = pos->fix_status;
    if (pos->fix_status != 0U) {
        member->position_valid = 1U;
        member->latitude_e6 = pos->latitude_e6;
        member->longitude_e6 = pos->longitude_e6;
        member->speed_cms = pos->speed_cms;
        member->heading_deg = pos->heading_deg;
    }
    member->sat_count = pos->sat_count;
    member->relay_allowed = node->cfg.relay_allowed;
    member->relay_tier = node->cfg.relay_tier;
    member->max_downstream = node->cfg.max_downstream;
    member->parent_id = node->cfg.role == SLE_TEAM_ROLE_MEMBER ? node->upstream_parent_id : 0U;
    member->next_hop_id = node->cfg.role == SLE_TEAM_ROLE_MEMBER ? node->upstream_parent_id : 0U;
    member->last_seen_s = sle_team_location_now(node);
    member->last_seq = node->next_seq;
    if (node->ops.on_position != NULL) {
        node->ops.on_position(node->ops.user_ctx, node->cfg.self_id, pos);
    }
    return SLE_TEAM_OK;
}
