#include "sle_team_web_api.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char *buf;
    size_t len;
    size_t used;
    int truncated;
} sle_team_json_writer_t;

/* Append-only JSON writer. Once truncated, later appends become no-ops. */
static void json_append(sle_team_json_writer_t *writer, const char *fmt, ...)
{
    va_list ap;
    int written;

    if (writer == NULL || writer->buf == NULL || writer->len == 0U || writer->truncated != 0) {
        return;
    }
    if (writer->used >= writer->len) {
        writer->truncated = 1;
        return;
    }

    va_start(ap, fmt);
    written = vsnprintf(&writer->buf[writer->used], writer->len - writer->used, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= writer->len - writer->used) {
        writer->used = writer->len - 1U;
        writer->buf[writer->used] = '\0';
        writer->truncated = 1;
        return;
    }
    writer->used += (size_t)written;
}

/* Escape only what the JSON/event summaries can contain on the MCU. */
static void json_append_escaped(sle_team_json_writer_t *writer, const char *text)
{
    const unsigned char *p;

    json_append(writer, "\"");
    if (text != NULL) {
        for (p = (const unsigned char *)text; *p != '\0'; p++) {
            switch (*p) {
                case '\"':
                    json_append(writer, "\\\"");
                    break;
                case '\\':
                    json_append(writer, "\\\\");
                    break;
                case '\n':
                    json_append(writer, "\\n");
                    break;
                case '\r':
                    json_append(writer, "\\r");
                    break;
                case '\t':
                    json_append(writer, "\\t");
                    break;
                default:
                    if (*p < 0x20U) {
                        json_append(writer, "\\u%04x", *p);
                    } else {
                        json_append(writer, "%c", *p);
                    }
                    break;
            }
        }
    }
    json_append(writer, "\"");
}

/* Web UI shows both full MAC and a short suffix label when the MAC is known. */
static void json_append_mac_fields(sle_team_json_writer_t *writer, const uint8_t mac[6], uint8_t mac_ready)
{
    if (mac_ready == 0U || mac == NULL) {
        json_append(writer, "\"macReady\":false,\"macSuffix\":\"\"");
        return;
    }
    json_append(writer,
        "\"macReady\":true,\"mac\":\"%02X:%02X:%02X:%02X:%02X:%02X\",\"macSuffix\":\"%02X%02X\"",
        mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], mac[4], mac[5]);
}

static uint8_t web_status_online_node_count(const sle_team_node_t *node)
{
    uint8_t i;
    uint8_t count = 0U;

    if (node == NULL) {
        return 0U;
    }
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        count = 1U;
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            if (node->members[i].online != 0U) {
                count++;
            }
        }
        return count;
    }
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER) {
        return node->joined != 0U ? 1U : 0U;
    }
    return 0U;
}

static uint8_t web_status_relay_node_count(const sle_team_node_t *node)
{
    uint8_t i;
    uint8_t count = 0U;

    if (node == NULL) {
        return 0U;
    }
    if (node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            if (node->members[i].online != 0U && node->members[i].relay_allowed != 0U) {
                count++;
            }
        }
        return count;
    }
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER) {
        return (node->joined != 0U && (node->cfg.relay_enabled != 0U || node->cfg.relay_allowed != 0U)) ? 1U : 0U;
    }
    return 0U;
}

const char *sle_team_web_role_name(uint8_t role)
{
    return role == (uint8_t)SLE_TEAM_ROLE_LEADER ? "leader" : "member";
}

const char *sle_team_web_state_name(uint8_t state)
{
    switch (state) {
        case SLE_TEAM_NET_WAIT_POLICY:
            return "wait_policy";
        case SLE_TEAM_NET_JOINING:
            return "joining";
        case SLE_TEAM_NET_ONLINE:
            return "online";
        case SLE_TEAM_NET_IDLE:
        default:
            return "idle";
    }
}

const char *sle_team_web_parent_state_name(uint8_t state)
{
    switch (state) {
        case SLE_TEAM_PARENT_WAIT_POLICY:
            return "wait_policy";
        case SLE_TEAM_PARENT_CONNECTED:
            return "connected";
        case SLE_TEAM_PARENT_IDLE:
        default:
            return "idle";
    }
}

const char *sle_team_web_msg_type_name(uint8_t app_msg_type)
{
    switch (app_msg_type) {
        case SLE_TEAM_APP_HELLO:
            return "HELLO";
        case SLE_TEAM_APP_HEARTBEAT:
            return "HEARTBEAT";
        case SLE_TEAM_APP_POS_REPORT:
            return "POS_REPORT";
        case SLE_TEAM_APP_ALERT:
            return "ALERT";
        case SLE_TEAM_APP_CONFIG:
            return "CONFIG";
        case SLE_TEAM_APP_ACK:
            return "ACK";
        case SLE_TEAM_APP_ROUTE_UPDATE:
            return "ROUTE_UPDATE";
        default:
            return "UNKNOWN";
    }
}

void sle_team_web_event_log_init(sle_team_web_event_log_t *log)
{
    if (log == NULL) {
        return;
    }
    (void)memset(log, 0, sizeof(*log));
    log->next_id = 1U;
}

/* Push newest event into a small ring; readers walk backward from head. */
void sle_team_web_event_push(sle_team_web_event_log_t *log, uint32_t time_s,
    sle_team_web_event_direction_t direction, uint8_t app_msg_type, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const char *summary)
{
    sle_team_web_event_t *event;

    if (log == NULL) {
        return;
    }
    event = &log->events[log->head];
    (void)memset(event, 0, sizeof(*event));
    event->id = log->next_id++;
    event->time_s = time_s;
    event->direction = direction;
    event->app_msg_type = app_msg_type;
    event->src_id = src_id;
    event->dst_id = dst_id;
    event->seq = seq;
    if (summary != NULL) {
        (void)snprintf(event->summary, sizeof(event->summary), "%s", summary);
    }
    log->head = (uint8_t)((log->head + 1U) % SLE_TEAM_WEB_EVENT_COUNT);
    if (log->count < SLE_TEAM_WEB_EVENT_COUNT) {
        log->count++;
    }
}

int sle_team_web_write_status_json(const sle_team_node_t *node, uint32_t uptime_s, const char *transport,
    char *out, size_t out_len)
{
    sle_team_json_writer_t writer;
    uint8_t i;

    if (node == NULL || out == NULL || out_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    writer.buf = out;
    writer.len = out_len;
    writer.used = 0U;
    writer.truncated = 0;
    out[0] = '\0';

    /* Status is the local node summary plus pairing/member-filter controls. */
    json_append(&writer,
        "{\"teamId\":%u,\"selfId\":%u,\"leaderId\":%u,",
        node->cfg.team_id, node->cfg.self_id, node->cfg.leader_id);
    json_append_mac_fields(&writer, node->cfg.self_mac, node->cfg.self_mac_ready);
    json_append(&writer,
        ",\"role\":\"%s\",\"state\":\"%s\","
        "\"joined\":%s,\"relayAllowed\":%s,\"relayEnabled\":%s,\"relayTier\":%u,\"maxDownstream\":%u,"
        "\"upstreamParentId\":%u,\"upstreamParentState\":\"%s\","
        "\"nextSeq\":%u,\"uptimeS\":%lu,\"transport\":\"%s\","
        "\"pairingEnabled\":%s,\"memberFilterEnabled\":%s,\"allowedMemberCount\":%u,\"allowedMembers\":[",
        sle_team_web_role_name((uint8_t)node->cfg.role),
        sle_team_web_state_name((uint8_t)node->state), node->joined != 0U ? "true" : "false",
        node->cfg.relay_allowed != 0U ? "true" : "false",
        node->cfg.relay_enabled != 0U ? "true" : "false",
        node->cfg.relay_tier, node->cfg.max_downstream,
        node->upstream_parent_id,
        sle_team_web_parent_state_name((uint8_t)node->upstream_parent_state),
        node->next_seq,
        (unsigned long)uptime_s, transport != NULL ? transport : "ws63-http",
        node->cfg.pairing_enabled != 0U ? "true" : "false",
        node->cfg.member_filter_enabled != 0U ? "true" : "false", node->cfg.allowed_member_count);
    for (i = 0U; i < node->cfg.allowed_member_count && i < SLE_TEAM_MAX_MEMBERS; i++) {
        json_append(&writer, "%s%u", i == 0U ? "" : ",", node->cfg.allowed_member_ids[i]);
    }
    json_append(&writer,
        "],\"onlineNodeCount\":%u,\"relayNodeCount\":%u",
        web_status_online_node_count(node), web_status_relay_node_count(node));
    json_append(&writer, "}");
    return writer.truncated != 0 ? SLE_TEAM_ERR_BUF : (int)writer.used;
}

int sle_team_web_write_pending_json(const sle_team_node_t *node, char *out, size_t out_len)
{
    sle_team_json_writer_t writer;
    uint8_t i;
    uint8_t wrote = 0U;

    if (node == NULL || out == NULL || out_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    writer.buf = out;
    writer.len = out_len;
    writer.used = 0U;
    writer.truncated = 0;
    out[0] = '\0';

    json_append(&writer, "[");
    /* Pending entries are HELLOs waiting for pairing approval. */
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_pending_member_t *member = &node->pending_members[i];
        if (member->active == 0U) {
            continue;
        }
        if (wrote != 0U) {
            json_append(&writer, ",");
        }
        json_append(&writer,
            "{\"id\":%u,\"role\":\"%s\",\"batteryPercent\":%u,",
            member->member_id, sle_team_web_role_name(member->role), member->battery_percent);
        json_append_mac_fields(&writer, member->mac, member->mac_ready);
        json_append(&writer,
            ",\"lastSeenS\":%lu}",
            (unsigned long)member->last_seen_s);
        wrote = 1U;
    }
    json_append(&writer, "]");
    return writer.truncated != 0 ? SLE_TEAM_ERR_BUF : (int)writer.used;
}

int sle_team_web_write_nodes_json(const sle_team_node_t *node, char *out, size_t out_len)
{
    sle_team_json_writer_t writer;
    uint8_t i;
    uint8_t wrote = 0U;

    if (node == NULL || out == NULL || out_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    writer.buf = out;
    writer.len = out_len;
    writer.used = 0U;
    writer.truncated = 0;
    out[0] = '\0';

    json_append(&writer, "[");
    /*
     * A member's local Web/AP page does not own the leader member table, so
     * expose a self row first. Without this, member /api/nodes is empty even
     * while the board is joined and useful.
     */
    if (node->cfg.role == SLE_TEAM_ROLE_MEMBER) {
        const sle_team_member_record_t *self = sle_team_node_find_member(node, node->cfg.self_id);
        uint8_t self_position_valid = self != NULL ? self->position_valid : 0U;
        uint8_t self_fix_status = self != NULL ? self->fix_status : 0U;
        uint8_t self_battery = self != NULL ? self->battery_percent : 0U;
        uint8_t self_sat_count = self != NULL ? self->sat_count : 0U;
        int32_t self_latitude_e6 = self != NULL ? self->latitude_e6 : 0;
        int32_t self_longitude_e6 = self != NULL ? self->longitude_e6 : 0;
        uint16_t self_speed_cms = self != NULL ? self->speed_cms : 0U;
        uint16_t self_heading_deg = self != NULL ? self->heading_deg : 0U;
        uint32_t self_last_seen_s = self != NULL ? self->last_seen_s : 0U;
        json_append(&writer,
            "{\"id\":%u,\"role\":\"member\",\"self\":true,\"online\":%s,"
            "\"policyPending\":false,\"batteryPercent\":%u,\"fixStatus\":%u,",
            node->cfg.self_id, node->joined != 0U ? "true" : "false", self_battery, self_fix_status);
        json_append_mac_fields(&writer, node->cfg.self_mac, node->cfg.self_mac_ready);
        json_append(&writer,
            ",\"positionValid\":%s,\"latitudeE6\":%ld,\"longitudeE6\":%ld,"
            "\"speedCms\":%u,\"headingDeg\":%u,\"satCount\":%u,\"lastRssiDbm\":null,"
            "\"relayAllowed\":%s,\"relayTier\":%u,\"maxDownstream\":%u,"
            "\"parentId\":%u,\"nextHopId\":%u,\"childCount\":0,"
            "\"lastSeq\":%u,\"lastSeenS\":%lu}",
            self_position_valid != 0U ? "true" : "false",
            (long)self_latitude_e6, (long)self_longitude_e6,
            self_speed_cms, self_heading_deg, self_sat_count,
            node->cfg.relay_allowed != 0U ? "true" : "false", node->cfg.relay_tier,
            node->cfg.max_downstream, node->upstream_parent_id, node->upstream_parent_id,
            node->next_seq, (unsigned long)self_last_seen_s);
        wrote = 1U;
    }
    /*
     * Keep offline records in the JSON if they still have pending policy or a
     * last-known position. The Web UI needs that to show lost nodes on the map.
     */
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &node->members[i];
        if (member->online == 0U && member->policy_pending == 0U && member->position_valid == 0U) {
            continue;
        }
        if (node->cfg.role == SLE_TEAM_ROLE_MEMBER && member->member_id == node->cfg.self_id) {
            continue;
        }
        if (wrote != 0U) {
            json_append(&writer, ",");
        }
        json_append(&writer,
            "{\"id\":%u,\"role\":\"%s\",\"online\":%s,\"policyPending\":%s,\"batteryPercent\":%u,\"fixStatus\":%u,",
            member->member_id, sle_team_web_role_name(member->role), member->online != 0U ? "true" : "false",
            member->policy_pending != 0U ? "true" : "false", member->battery_percent, member->fix_status);
        json_append_mac_fields(&writer, member->mac, member->mac_ready);
        json_append(&writer,
            ",\"positionValid\":%s,\"latitudeE6\":%ld,\"longitudeE6\":%ld,\"speedCms\":%u,\"headingDeg\":%u,\"satCount\":%u",
            member->position_valid != 0U ? "true" : "false",
            (long)member->latitude_e6, (long)member->longitude_e6,
            member->speed_cms, member->heading_deg, member->sat_count);
        if (member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN) {
            json_append(&writer, ",\"lastRssiDbm\":null");
        } else {
            json_append(&writer, ",\"lastRssiDbm\":%d", member->last_rssi_dbm);
        }
        json_append(&writer,
            ",\"relayAllowed\":%s,\"relayTier\":%u,\"maxDownstream\":%u,"
            "\"parentId\":%u,\"nextHopId\":%u,\"childCount\":%u,"
            "\"lastSeq\":%u,\"lastSeenS\":%lu}",
            member->relay_allowed != 0U ? "true" : "false", member->relay_tier, member->max_downstream,
            member->parent_id, member->next_hop_id, member->child_count,
            member->last_seq, (unsigned long)member->last_seen_s);
        wrote = 1U;
    }
    json_append(&writer, "]");
    return writer.truncated != 0 ? SLE_TEAM_ERR_BUF : (int)writer.used;
}

int sle_team_web_write_events_json(const sle_team_web_event_log_t *log, char *out, size_t out_len)
{
    static const char *directions[] = {"rx", "tx", "system"};
    sle_team_json_writer_t writer;
    uint8_t i;

    if (log == NULL || out == NULL || out_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    writer.buf = out;
    writer.len = out_len;
    writer.used = 0U;
    writer.truncated = 0;
    out[0] = '\0';

    json_append(&writer, "[");
    /* Newest first: the UI can render the log without sorting on the browser. */
    for (i = 0U; i < log->count; i++) {
        uint8_t index = (uint8_t)((log->head + SLE_TEAM_WEB_EVENT_COUNT - 1U - i) % SLE_TEAM_WEB_EVENT_COUNT);
        const sle_team_web_event_t *event = &log->events[index];
        const char *direction = "system";
        if ((uint8_t)event->direction < (uint8_t)(sizeof(directions) / sizeof(directions[0]))) {
            direction = directions[event->direction];
        }
        if (i != 0U) {
            json_append(&writer, ",");
        }
        json_append(&writer,
            "{\"id\":\"evt-%lu\",\"time\":\"%lu\",\"direction\":\"%s\",\"type\":\"%s\","
            "\"srcId\":%u,\"dstId\":%u,\"seq\":%u,\"summary\":",
            (unsigned long)event->id, (unsigned long)event->time_s, direction,
            sle_team_web_msg_type_name(event->app_msg_type), event->src_id, event->dst_id, event->seq);
        json_append_escaped(&writer, event->summary);
        json_append(&writer, "}");
    }
    json_append(&writer, "]");
    return writer.truncated != 0 ? SLE_TEAM_ERR_BUF : (int)writer.used;
}
