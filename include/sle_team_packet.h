#ifndef SLE_TEAM_PACKET_H
#define SLE_TEAM_PACKET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SLE_TEAM_MAX_PATH_SIZE 64U
#define SLE_TEAM_MAX_PAYLOAD_SIZE 184U
#define SLE_TEAM_MAX_PACKET_SIZE (1U + 4U + 1U + SLE_TEAM_MAX_PATH_SIZE + SLE_TEAM_MAX_PAYLOAD_SIZE)
#define SLE_TEAM_RSSI_UNKNOWN 127
#define SLE_TEAM_LEADER_TERM_DEFAULT 1U
#define SLE_TEAM_FW_COMPAT_ANY 0U

typedef enum {
    SLE_TEAM_ROUTE_TRANSPORT_FLOOD = 0x00,
    SLE_TEAM_ROUTE_FLOOD = 0x01,
    SLE_TEAM_ROUTE_DIRECT = 0x02,
    SLE_TEAM_ROUTE_TRANSPORT_DIRECT = 0x03,
} sle_team_route_type_t;

typedef enum {
    SLE_TEAM_PKT_REQ = 0x00,
    SLE_TEAM_PKT_RESPONSE = 0x01,
    SLE_TEAM_PKT_TEXT = 0x02,
    SLE_TEAM_PKT_ACK = 0x03,
    SLE_TEAM_PKT_ADVERT = 0x04,
    SLE_TEAM_PKT_GROUP_TEXT = 0x05,
    SLE_TEAM_PKT_GROUP_DATA = 0x06,
    SLE_TEAM_PKT_ANON_REQ = 0x07,
    SLE_TEAM_PKT_PATH = 0x08,
    SLE_TEAM_PKT_TRACE = 0x09,
    SLE_TEAM_PKT_MULTIPART = 0x0A,
    SLE_TEAM_PKT_CONTROL = 0x0B,
    SLE_TEAM_PKT_RAW_CUSTOM = 0x0F,
} sle_team_payload_type_t;

typedef enum {
    SLE_TEAM_PAYLOAD_V1 = 0x00,
} sle_team_payload_version_t;

typedef enum {
    SLE_TEAM_APP_HELLO = 0x01,
    SLE_TEAM_APP_HEARTBEAT = 0x02,
    SLE_TEAM_APP_POS_REPORT = 0x03,
    SLE_TEAM_APP_ALERT = 0x04,
    SLE_TEAM_APP_CONFIG = 0x05,
    SLE_TEAM_APP_ACK = 0x06,
    SLE_TEAM_APP_ROUTE_UPDATE = 0x07,
} sle_team_app_msg_type_t;

typedef enum {
    SLE_TEAM_ALERT_NONE = 0,
    SLE_TEAM_ALERT_DISTANCE = 1,
    SLE_TEAM_ALERT_TIMEOUT = 2,
    SLE_TEAM_ALERT_LOW_BATTERY = 3,
    SLE_TEAM_ALERT_LEAVE = 4,
} sle_team_alert_reason_t;

typedef enum {
    SLE_TEAM_OK = 0,
    SLE_TEAM_ERR_ARG = -1,
    SLE_TEAM_ERR_BUF = -2,
    SLE_TEAM_ERR_FORMAT = -3,
    SLE_TEAM_ERR_UNSUPPORTED = -4,
} sle_team_status_t;

typedef struct {
    uint8_t version;
    uint8_t payload_type;
    uint8_t route_type;
    uint16_t transport_code_1;
    uint16_t transport_code_2;
    bool has_transport_codes;
    uint8_t path_hash_size;
    uint8_t hop_count;
    uint8_t path[SLE_TEAM_MAX_PATH_SIZE];
    uint16_t payload_len;
    uint8_t payload[SLE_TEAM_MAX_PAYLOAD_SIZE];
} sle_team_mesh_packet_t;

typedef struct {
    uint8_t app_msg_type;
    uint8_t flags;
    uint16_t seq;
    uint8_t team_id;
    uint8_t src_id;
    uint8_t dst_id;
    uint8_t ttl;
    uint16_t leader_term;
    uint16_t body_len;
    const uint8_t *body;
} sle_team_app_packet_t;

typedef struct {
    uint8_t device_id;
    uint8_t role;
    uint8_t battery_percent;
    uint8_t mac[6];
    uint8_t mac_ready;
    uint8_t fw_compat_lo;
    uint8_t fw_compat_hi;
} sle_team_hello_body_t;

typedef struct {
    uint8_t battery_percent;
    int8_t rssi_dbm;
    uint8_t fix_status;
    uint8_t reserved;
} sle_team_heartbeat_body_t;

typedef struct {
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint16_t speed_cms;
    uint16_t heading_deg;
    uint8_t battery_percent;
    uint8_t fix_status;
    uint8_t sat_count;
    uint8_t reserved;
} sle_team_pos_body_t;

typedef struct {
    uint8_t lost_member_id;
    uint8_t reason;
    uint16_t reserved;
    int32_t last_latitude_e6;
    int32_t last_longitude_e6;
    uint32_t last_report_s;
} sle_team_alert_body_t;

typedef struct {
    uint16_t report_interval_s;
    uint16_t warn_distance_m;
    uint16_t lost_distance_m;
    uint16_t heartbeat_timeout_s;
    uint8_t relay_allowed;
    uint8_t relay_tier;
    uint8_t max_downstream;
    uint8_t reserved;
} sle_team_config_body_t;

#define SLE_TEAM_CONFIG_FLAG_RELAY_DISCOVERY_ONLY 0x01U
#define SLE_TEAM_ROUTE_UPDATE_FLAG_RELAY_GRANT 0x01U

typedef struct {
    uint16_t ack_seq;
    uint8_t acked_msg_type;
    uint8_t status_code;
} sle_team_ack_body_t;

typedef struct {
    uint8_t parent_id;
    uint8_t next_hop_id;
    uint8_t parent_state;
    uint8_t reserved; /* bit0: leader grants relay-enable sync */
} sle_team_route_update_body_t;

uint8_t sle_team_make_header(uint8_t version, uint8_t payload_type, uint8_t route_type);
uint8_t sle_team_make_path_length(uint8_t hop_count, uint8_t path_hash_size);

int sle_team_encode_mesh_packet(const sle_team_mesh_packet_t *packet, uint8_t *out_buf, size_t out_buf_len,
    size_t *out_len);
int sle_team_decode_mesh_packet(sle_team_mesh_packet_t *packet, const uint8_t *buf, size_t buf_len);

int sle_team_encode_app_packet(const sle_team_app_packet_t *packet, uint8_t *out_buf, size_t out_buf_len,
    uint16_t *out_len);
int sle_team_decode_app_packet(sle_team_app_packet_t *packet, const uint8_t *buf, size_t buf_len);

int sle_team_build_hello(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_hello_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_heartbeat(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_heartbeat_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_pos_report(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_pos_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_alert(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_alert_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_config(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_config_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_ack(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_ack_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);
int sle_team_build_route_update(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_route_update_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len);

uint8_t sle_team_scan_route_id_from_data(const uint8_t *data, uint16_t len);
uint16_t sle_team_scan_fw_compat_from_data(const uint8_t *data, uint16_t len);

int sle_team_wrap_mesh_group_data(const uint8_t channel_hash, const uint8_t cipher_mac[2],
    const uint8_t *app_payload, uint16_t app_payload_len, sle_team_route_type_t route_type,
    sle_team_mesh_packet_t *packet);
int sle_team_unwrap_mesh_group_data(const sle_team_mesh_packet_t *packet, uint8_t *channel_hash,
    uint8_t cipher_mac[2], const uint8_t **app_payload, uint16_t *app_payload_len);

#ifdef __cplusplus
}
#endif

#endif
