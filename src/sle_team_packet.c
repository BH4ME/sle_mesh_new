#include "sle_team_packet.h"

#include <string.h>

#define SLE_TEAM_APP_HEADER_SIZE 12U
#define SLE_TEAM_GRP_DATA_WRAPPER_SIZE 3U
#define SLE_TEAM_ADV_ROUTE_HINT_TYPE 0xFFU
#define SLE_TEAM_ADV_ROUTE_MAGIC_0 0x53U
#define SLE_TEAM_ADV_ROUTE_MAGIC_1 0x4CU
#define SLE_TEAM_ADV_ROUTE_HINT_LEN 6U
#define SLE_TEAM_ADV_ROUTE_HINT_TOTAL_LEN 7U
#define SLE_TEAM_SCAN_BROADCAST_ID 0xFFU

/* Little-endian helpers keep the wire format stable across the MCU. */
static void sle_team_put_u16_le(uint8_t *buf, uint16_t value)
{
    buf[0] = (uint8_t)(value & 0xFFU);
    buf[1] = (uint8_t)((value >> 8) & 0xFFU);
}

/* Read the same little-endian encoding back from the wire. */
static uint16_t sle_team_get_u16_le(const uint8_t *buf)
{
    return (uint16_t)buf[0] | ((uint16_t)buf[1] << 8);
}

/*
 * Build a fixed-size app message body.
 *
 * Most packet builders are simple wrappers around this helper: fill the app
 * header fields, then encode the packet into bytes for the transport layer.
 */
static int sle_team_build_fixed_body(uint8_t app_msg_type, uint8_t team_id, uint8_t src_id, uint8_t dst_id,
    uint16_t seq, const uint8_t *body, uint16_t body_len, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    sle_team_app_packet_t packet;

    if (out_buf == NULL || out_len == NULL) {
        return SLE_TEAM_ERR_ARG;
    }

    packet.app_msg_type = app_msg_type;
    packet.flags = 0;
    packet.seq = seq;
    packet.team_id = team_id;
    packet.src_id = src_id;
    packet.dst_id = dst_id;
    packet.ttl = 1;
    packet.leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    packet.body_len = body_len;
    packet.body = body;
    return sle_team_encode_app_packet(&packet, out_buf, out_buf_len, out_len);
}

/* Pack version/payload/route into the compact 1-byte mesh header. */
uint8_t sle_team_make_header(uint8_t version, uint8_t payload_type, uint8_t route_type)
{
    return (uint8_t)(((version & 0x03U) << 6) | ((payload_type & 0x0FU) << 2) | (route_type & 0x03U));
}

/* Encode hop count and hash width for the routing/path side channel. */
uint8_t sle_team_make_path_length(uint8_t hop_count, uint8_t path_hash_size)
{
    uint8_t hash_code;

    if (path_hash_size == 0U || path_hash_size > 3U || hop_count > 63U) {
        return 0U;
    }

    hash_code = (uint8_t)(path_hash_size - 1U);
    return (uint8_t)((hash_code << 6) | (hop_count & 0x3FU));
}

/* Serialize the outer mesh wrapper used for direct and relay forwarding. */
int sle_team_encode_mesh_packet(const sle_team_mesh_packet_t *packet, uint8_t *out_buf, size_t out_buf_len,
    size_t *out_len)
{
    size_t offset;
    size_t path_len;
    uint8_t header;

    if (packet == NULL || out_buf == NULL || out_len == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    if (packet->payload_len > SLE_TEAM_MAX_PAYLOAD_SIZE) {
        return SLE_TEAM_ERR_ARG;
    }
    if (packet->path_hash_size > 3U || packet->hop_count > 63U) {
        return SLE_TEAM_ERR_ARG;
    }

    path_len = (size_t)packet->path_hash_size * (size_t)packet->hop_count;
    if (path_len > SLE_TEAM_MAX_PATH_SIZE) {
        return SLE_TEAM_ERR_ARG;
    }

    offset = 0U;
    header = sle_team_make_header(packet->version, packet->payload_type, packet->route_type);
    if (header == 0U && (packet->version != 0U || packet->payload_type != 0U || packet->route_type != 0U)) {
        return SLE_TEAM_ERR_ARG;
    }

    if (out_buf_len < 1U + (packet->has_transport_codes ? 4U : 0U) + 1U + path_len + packet->payload_len) {
        return SLE_TEAM_ERR_BUF;
    }

    out_buf[offset++] = header;

    if (packet->has_transport_codes) {
        sle_team_put_u16_le(&out_buf[offset], packet->transport_code_1);
        offset += 2U;
        sle_team_put_u16_le(&out_buf[offset], packet->transport_code_2);
        offset += 2U;
    }

    out_buf[offset++] = sle_team_make_path_length(packet->hop_count, packet->path_hash_size == 0U ? 1U :
        packet->path_hash_size);

    if (path_len > 0U) {
        (void)memcpy(&out_buf[offset], packet->path, path_len);
        offset += path_len;
    }

    if (packet->payload_len > 0U) {
        (void)memcpy(&out_buf[offset], packet->payload, packet->payload_len);
        offset += packet->payload_len;
    }

    *out_len = offset;
    return SLE_TEAM_OK;
}

/* Parse the outer mesh wrapper before the app packet is decoded. */
int sle_team_decode_mesh_packet(sle_team_mesh_packet_t *packet, const uint8_t *buf, size_t buf_len)
{
    size_t offset;
    uint8_t header;
    uint8_t path_length;
    size_t path_len;

    if (packet == NULL || buf == NULL || buf_len < 2U) {
        return SLE_TEAM_ERR_ARG;
    }

    (void)memset(packet, 0, sizeof(*packet));
    offset = 0U;
    header = buf[offset++];

    packet->route_type = header & 0x03U;
    packet->payload_type = (header >> 2) & 0x0FU;
    packet->version = (header >> 6) & 0x03U;
    packet->has_transport_codes = (bool)((packet->route_type == SLE_TEAM_ROUTE_TRANSPORT_FLOOD) ||
        (packet->route_type == SLE_TEAM_ROUTE_TRANSPORT_DIRECT));

    if (packet->has_transport_codes) {
        if (buf_len < offset + 4U + 1U) {
            return SLE_TEAM_ERR_FORMAT;
        }
        packet->transport_code_1 = sle_team_get_u16_le(&buf[offset]);
        offset += 2U;
        packet->transport_code_2 = sle_team_get_u16_le(&buf[offset]);
        offset += 2U;
    }

    path_length = buf[offset++];
    packet->hop_count = path_length & 0x3FU;
    packet->path_hash_size = (uint8_t)(((path_length >> 6) & 0x03U) + 1U);
    if (packet->path_hash_size > 3U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }

    path_len = (size_t)packet->hop_count * (size_t)packet->path_hash_size;
    if (path_len > SLE_TEAM_MAX_PATH_SIZE || buf_len < offset + path_len) {
        return SLE_TEAM_ERR_FORMAT;
    }

    if (path_len > 0U) {
        (void)memcpy(packet->path, &buf[offset], path_len);
        offset += path_len;
    }

    packet->payload_len = (uint16_t)(buf_len - offset);
    if (packet->payload_len > SLE_TEAM_MAX_PAYLOAD_SIZE) {
        return SLE_TEAM_ERR_FORMAT;
    }

    if (packet->payload_len > 0U) {
        (void)memcpy(packet->payload, &buf[offset], packet->payload_len);
    }

    return SLE_TEAM_OK;
}

/* Serialize the inner team packet header plus its body bytes. */
int sle_team_encode_app_packet(const sle_team_app_packet_t *packet, uint8_t *out_buf, size_t out_buf_len,
    uint16_t *out_len)
{
    size_t total_len;

    if (packet == NULL || out_buf == NULL || out_len == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    if (packet->body_len > 0U && packet->body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }

    total_len = SLE_TEAM_APP_HEADER_SIZE + packet->body_len;
    if (out_buf_len < total_len || total_len > UINT16_MAX) {
        return SLE_TEAM_ERR_BUF;
    }

    out_buf[0] = packet->app_msg_type;
    out_buf[1] = packet->flags;
    sle_team_put_u16_le(&out_buf[2], packet->seq);
    out_buf[4] = packet->team_id;
    out_buf[5] = packet->src_id;
    out_buf[6] = packet->dst_id;
    out_buf[7] = packet->ttl;
    sle_team_put_u16_le(&out_buf[8], packet->leader_term);
    sle_team_put_u16_le(&out_buf[10], packet->body_len);

    if (packet->body_len > 0U) {
        (void)memcpy(&out_buf[SLE_TEAM_APP_HEADER_SIZE], packet->body, packet->body_len);
    }

    *out_len = (uint16_t)total_len;
    return SLE_TEAM_OK;
}

/* Recover the inner packet fields and point body at the decoded payload. */
int sle_team_decode_app_packet(sle_team_app_packet_t *packet, const uint8_t *buf, size_t buf_len)
{
    uint16_t body_len;

    if (packet == NULL || buf == NULL || buf_len < SLE_TEAM_APP_HEADER_SIZE) {
        return SLE_TEAM_ERR_ARG;
    }

    body_len = sle_team_get_u16_le(&buf[10]);
    if (buf_len < SLE_TEAM_APP_HEADER_SIZE + body_len) {
        return SLE_TEAM_ERR_FORMAT;
    }

    packet->app_msg_type = buf[0];
    packet->flags = buf[1];
    packet->seq = sle_team_get_u16_le(&buf[2]);
    packet->team_id = buf[4];
    packet->src_id = buf[5];
    packet->dst_id = buf[6];
    packet->ttl = buf[7];
    packet->leader_term = sle_team_get_u16_le(&buf[8]);
    packet->body_len = body_len;
    packet->body = &buf[SLE_TEAM_APP_HEADER_SIZE];
    return SLE_TEAM_OK;
}

/* Message builders below are thin wrappers around the common app header. */
int sle_team_build_hello(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_hello_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_HELLO, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_heartbeat(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_heartbeat_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_HEARTBEAT, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_pos_report(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_pos_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_POS_REPORT, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_alert(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_alert_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_ALERT, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_config(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_config_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_CONFIG, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_ack(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_ack_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_ACK, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

int sle_team_build_route_update(uint8_t team_id, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const sle_team_route_update_body_t *body, uint8_t *out_buf, size_t out_buf_len, uint16_t *out_len)
{
    if (body == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    return sle_team_build_fixed_body(SLE_TEAM_APP_ROUTE_UPDATE, team_id, src_id, dst_id, seq,
        (const uint8_t *)body, (uint16_t)sizeof(*body), out_buf, out_buf_len, out_len);
}

/*
 * SLE advertisements carry a private route-hint block:
 *   len,type,magic0,magic1,route_id,fw_compat_lo,fw_compat_hi
 * Scan-time route and firmware filtering both start from this block.
 */
static uint16_t sle_team_find_adv_route_hint(const uint8_t *data, uint16_t len)
{
    uint16_t index;

    if (data == NULL || len < SLE_TEAM_ADV_ROUTE_HINT_TOTAL_LEN) {
        return UINT16_MAX;
    }
    for (index = 0U; (uint16_t)(index + SLE_TEAM_ADV_ROUTE_HINT_TOTAL_LEN - 1U) < len; index++) {
        if (data[index] == SLE_TEAM_ADV_ROUTE_HINT_LEN &&
            data[index + 1U] == SLE_TEAM_ADV_ROUTE_HINT_TYPE &&
            data[index + 2U] == SLE_TEAM_ADV_ROUTE_MAGIC_0 &&
            data[index + 3U] == SLE_TEAM_ADV_ROUTE_MAGIC_1) {
            return index;
        }
    }
    return UINT16_MAX;
}

/* Return 0 when the advertisement does not expose a usable route id. */
uint8_t sle_team_scan_route_id_from_data(const uint8_t *data, uint16_t len)
{
    uint16_t index = sle_team_find_adv_route_hint(data, len);
    uint8_t route_id;

    if (index == UINT16_MAX) {
        return 0U;
    }
    route_id = data[index + 4U];
    if (route_id != 0U && route_id != SLE_TEAM_SCAN_BROADCAST_ID) {
        return route_id;
    }
    return 0U;
}

/* Missing fingerprint means "unknown/any"; concrete mismatches are rejected later. */
uint16_t sle_team_scan_fw_compat_from_data(const uint8_t *data, uint16_t len)
{
    uint16_t index = sle_team_find_adv_route_hint(data, len);

    if (index == UINT16_MAX) {
        return SLE_TEAM_FW_COMPAT_ANY;
    }
    return (uint16_t)(((uint16_t)data[index + 6U] << 8U) | data[index + 5U]);
}

/* Add the group-data mini wrapper before encoding the outer mesh packet. */
int sle_team_wrap_mesh_group_data(const uint8_t channel_hash, const uint8_t cipher_mac[2],
    const uint8_t *app_payload, uint16_t app_payload_len, sle_team_route_type_t route_type,
    sle_team_mesh_packet_t *packet)
{
    if (cipher_mac == NULL || packet == NULL || app_payload == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    if ((uint32_t)app_payload_len + SLE_TEAM_GRP_DATA_WRAPPER_SIZE > SLE_TEAM_MAX_PAYLOAD_SIZE) {
        return SLE_TEAM_ERR_BUF;
    }

    (void)memset(packet, 0, sizeof(*packet));
    packet->version = SLE_TEAM_PAYLOAD_V1;
    packet->payload_type = SLE_TEAM_PKT_GROUP_DATA;
    packet->route_type = (uint8_t)route_type;
    packet->has_transport_codes = (bool)((route_type == SLE_TEAM_ROUTE_TRANSPORT_FLOOD) ||
        (route_type == SLE_TEAM_ROUTE_TRANSPORT_DIRECT));
    packet->path_hash_size = 1U;
    packet->hop_count = 0U;
    packet->payload[0] = channel_hash;
    packet->payload[1] = cipher_mac[0];
    packet->payload[2] = cipher_mac[1];
    (void)memcpy(&packet->payload[3], app_payload, app_payload_len);
    packet->payload_len = (uint16_t)(app_payload_len + SLE_TEAM_GRP_DATA_WRAPPER_SIZE);
    return SLE_TEAM_OK;
}

/* Remove the group-data mini wrapper and expose the embedded app packet. */
int sle_team_unwrap_mesh_group_data(const sle_team_mesh_packet_t *packet, uint8_t *channel_hash,
    uint8_t cipher_mac[2], const uint8_t **app_payload, uint16_t *app_payload_len)
{
    if (packet == NULL || channel_hash == NULL || cipher_mac == NULL || app_payload == NULL || app_payload_len == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    if (packet->payload_type != SLE_TEAM_PKT_GROUP_DATA || packet->payload_len < SLE_TEAM_GRP_DATA_WRAPPER_SIZE) {
        return SLE_TEAM_ERR_FORMAT;
    }

    *channel_hash = packet->payload[0];
    cipher_mac[0] = packet->payload[1];
    cipher_mac[1] = packet->payload[2];
    *app_payload = &packet->payload[3];
    *app_payload_len = (uint16_t)(packet->payload_len - SLE_TEAM_GRP_DATA_WRAPPER_SIZE);
    return SLE_TEAM_OK;
}
