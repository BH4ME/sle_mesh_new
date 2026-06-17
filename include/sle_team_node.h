#ifndef SLE_TEAM_NODE_H
#define SLE_TEAM_NODE_H

#include "sle_team_packet.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLE_TEAM_MAX_LOGICAL_MEMBERS 30U
#define SLE_TEAM_MAX_DIRECT_CONNECTIONS 8U
#define SLE_TEAM_MAX_MEMBERS SLE_TEAM_MAX_LOGICAL_MEMBERS
#define SLE_TEAM_NODE_TX_BUF_SIZE 256U
#define SLE_TEAM_BROADCAST_ID 0xFFU

typedef enum {
    SLE_TEAM_ROLE_MEMBER = 0,
    SLE_TEAM_ROLE_LEADER = 1,
} sle_team_node_role_t;

typedef enum {
    SLE_TEAM_NET_IDLE = 0,
    SLE_TEAM_NET_WAIT_POLICY = 1,
    SLE_TEAM_NET_JOINING = 2,
    SLE_TEAM_NET_ONLINE = 3,
} sle_team_network_state_t;

typedef enum {
    SLE_TEAM_PARENT_IDLE = 0,
    SLE_TEAM_PARENT_WAIT_POLICY = 1,
    SLE_TEAM_PARENT_CONNECTED = 2,
} sle_team_parent_state_t;

typedef enum {
    SLE_TEAM_SEND_UNICAST = 0,
    SLE_TEAM_SEND_GROUP = 1,
} sle_team_send_kind_t;

typedef struct {
    uint8_t member_id;
    uint8_t role;
    uint8_t battery_percent;
    uint8_t online;
    uint8_t policy_pending;
    uint8_t relay_recovery_candidate;
    uint8_t fix_status;
    uint8_t position_valid;
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint16_t speed_cms;
    uint16_t heading_deg;
    uint8_t sat_count;
    uint8_t mac[6];
    uint8_t mac_ready;
    uint8_t relay_allowed;
    uint8_t relay_tier;
    uint8_t max_downstream;
    uint8_t parent_id;
    uint8_t next_hop_id;
    uint8_t child_count;
    int8_t last_rssi_dbm;
    uint16_t last_seq;
    uint16_t pending_ack_seq;
    uint32_t last_seen_s;
} sle_team_member_record_t;

typedef struct {
    uint8_t member_id;
    uint8_t role;
    uint8_t battery_percent;
    uint8_t active;
    uint8_t mac[6];
    uint8_t mac_ready;
    uint32_t last_seen_s;
} sle_team_pending_member_t;

typedef struct sle_team_node sle_team_node_t;

typedef int (*sle_team_send_fn)(void *user_ctx, sle_team_send_kind_t kind, uint8_t dst_id,
    const uint8_t *buf, uint16_t len);
typedef uint32_t (*sle_team_now_fn)(void *user_ctx);
typedef int8_t (*sle_team_rssi_fn)(void *user_ctx);
typedef uint8_t (*sle_team_battery_percent_fn)(void *user_ctx);
typedef void (*sle_team_log_fn)(void *user_ctx, const char *text);

typedef void (*sle_team_joined_cb)(void *user_ctx, uint8_t member_id);
typedef void (*sle_team_position_cb)(void *user_ctx, uint8_t member_id, const sle_team_pos_body_t *pos);
typedef void (*sle_team_alert_cb)(void *user_ctx, uint8_t member_id, uint8_t reason);
typedef void (*sle_team_relay_offline_cb)(void *user_ctx, uint8_t member_id);
typedef uint8_t (*sle_team_member_timeout_defer_cb)(void *user_ctx, uint8_t member_id,
    uint32_t now_s, uint32_t last_seen_s);

typedef struct {
    sle_team_send_fn send;
    sle_team_now_fn now_s;
    sle_team_rssi_fn rssi_dbm;
    sle_team_battery_percent_fn battery_percent;
    sle_team_log_fn log;
    sle_team_joined_cb on_joined;
    sle_team_position_cb on_position;
    sle_team_alert_cb on_alert;
    sle_team_relay_offline_cb on_relay_offline;
    sle_team_member_timeout_defer_cb should_defer_member_timeout;
    void *user_ctx;
} sle_team_node_ops_t;

typedef struct {
    uint8_t team_id;
    uint8_t self_id;
    uint8_t leader_id;
    uint16_t leader_term;
    uint8_t self_mac[6];
    uint8_t self_mac_ready;
    sle_team_node_role_t role;
    uint8_t channel_hash;
    uint8_t pairing_enabled;
    uint8_t member_filter_enabled;
    uint8_t relay_allowed;
    uint8_t relay_tier;
    uint8_t max_downstream;
    uint8_t relay_enabled;
    uint8_t relay_discovery_only;
    uint8_t default_ttl;
    uint16_t fw_compat;
    uint8_t allowed_member_count;
    uint8_t allowed_member_ids[SLE_TEAM_MAX_MEMBERS];
    uint16_t report_interval_s;
    uint16_t heartbeat_interval_s;
    uint16_t warn_distance_m;
    uint16_t lost_distance_m;
    uint16_t heartbeat_timeout_s;
    uint16_t parent_timeout_s;
} sle_team_node_cfg_t;

struct sle_team_node {
    sle_team_node_cfg_t cfg;
    sle_team_node_ops_t ops;
    sle_team_network_state_t state;
    uint16_t next_seq;
    uint32_t last_hello_s;
    uint32_t last_heartbeat_s;
    uint32_t last_config_s;
    uint32_t last_leader_seen_s;
    uint32_t last_parent_seen_s;
    uint8_t joined;
    uint8_t upstream_parent_id;
    sle_team_parent_state_t upstream_parent_state;
    uint8_t rx_ingress_relay_id;
    uint8_t relay_recovery_pending;
    uint8_t relay_recovery_lost_relay_id;
    uint8_t relay_recovery_selected_id;
    sle_team_member_record_t members[SLE_TEAM_MAX_MEMBERS];
    sle_team_pending_member_t pending_members[SLE_TEAM_MAX_MEMBERS];
};

int sle_team_node_init(sle_team_node_t *node, const sle_team_node_cfg_t *cfg, const sle_team_node_ops_t *ops);
void sle_team_node_tick(sle_team_node_t *node);
int sle_team_node_on_packet(sle_team_node_t *node, const uint8_t *buf, size_t buf_len);

int sle_team_node_send_hello(sle_team_node_t *node, uint8_t dst_id);
int sle_team_node_send_heartbeat(sle_team_node_t *node, uint8_t dst_id, uint8_t battery_percent,
    int8_t rssi_dbm, uint8_t fix_status);
int sle_team_node_send_position(sle_team_node_t *node, uint8_t dst_id, const sle_team_pos_body_t *pos);
int sle_team_node_record_local_position(sle_team_node_t *node, const sle_team_pos_body_t *pos);
int sle_team_node_send_alert(sle_team_node_t *node, uint8_t dst_id, const sle_team_alert_body_t *alert);
int sle_team_node_send_config(sle_team_node_t *node, uint8_t dst_id);
int sle_team_node_send_ack(sle_team_node_t *node, uint8_t dst_id, uint16_t ack_seq, uint8_t acked_msg_type,
    uint8_t status_code);
int sle_team_node_send_route_update(sle_team_node_t *node, uint8_t dst_id, uint8_t parent_id,
    uint8_t parent_state, uint8_t next_hop_id);

const sle_team_member_record_t *sle_team_node_find_member(const sle_team_node_t *node, uint8_t member_id);
uint8_t sle_team_node_is_member_allowed(const sle_team_node_t *node, uint8_t member_id);
int sle_team_node_allow_all_members(sle_team_node_t *node);
int sle_team_node_set_allowed_members(sle_team_node_t *node, const uint8_t *member_ids, uint8_t count);
int sle_team_node_add_allowed_member(sle_team_node_t *node, uint8_t member_id);
int sle_team_node_remove_allowed_member(sle_team_node_t *node, uint8_t member_id);
int sle_team_node_pairing_start(sle_team_node_t *node);
int sle_team_node_pairing_stop(sle_team_node_t *node);
int sle_team_node_pairing_approve(sle_team_node_t *node, uint8_t member_id);
int sle_team_node_pairing_approve_with_relay(sle_team_node_t *node, uint8_t member_id, uint8_t relay_allowed);
int sle_team_node_grant_relay(sle_team_node_t *node, uint8_t member_id);
int sle_team_node_member_select_leader(sle_team_node_t *node, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash);
int sle_team_node_member_select_leader_term(sle_team_node_t *node, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_term);
int sle_team_node_member_leave(sle_team_node_t *node);
int sle_team_node_member_link_lost(sle_team_node_t *node);
int sle_team_node_member_offline(sle_team_node_t *node, uint8_t member_id);

#ifdef __cplusplus
}
#endif

#endif
