#include "sle_team_packet.h"

#include <stdio.h>
#include <string.h>

/*
 * 主从节点第一版都先复用这套公共逻辑：
 * 1. 组织业务消息体
 * 2. 编成应用层包
 * 3. 再包成 MeshCore 风格的 group datagram
 *
 * 后面再按 leader/member 拆业务状态机。
 */

static uint16_t g_seq = 1;

static void dump_hex(const uint8_t *buf, uint16_t len)
{
    uint16_t i;

    for (i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
}

int build_demo_position_packet(uint8_t team_id, uint8_t node_id, uint8_t dst_id, uint8_t *out, size_t out_size,
    size_t *out_len)
{
    sle_team_pos_body_t pos;
    sle_team_mesh_packet_t mesh_packet;
    uint8_t app_buf[64];
    uint16_t app_len;
    uint8_t cipher_mac[2] = {0x00, 0x00};

    (void)memset(&pos, 0, sizeof(pos));
    pos.latitude_e6 = 39908456;
    pos.longitude_e6 = 116397128;
    pos.speed_cms = 120;
    pos.heading_deg = 90;
    pos.battery_percent = 88;
    pos.fix_status = 1;
    pos.sat_count = 9;

    if (sle_team_build_pos_report(team_id, node_id, dst_id, g_seq++, &pos, app_buf, sizeof(app_buf), &app_len) !=
        SLE_TEAM_OK) {
        return -1;
    }

    if (sle_team_wrap_mesh_group_data(0x11, cipher_mac, app_buf, app_len, SLE_TEAM_ROUTE_DIRECT, &mesh_packet) !=
        SLE_TEAM_OK) {
        return -2;
    }

    return sle_team_encode_mesh_packet(&mesh_packet, out, out_size, out_len);
}

int parse_demo_position_packet(const uint8_t *buf, size_t buf_len)
{
    sle_team_mesh_packet_t mesh_packet;
    sle_team_app_packet_t app_packet;
    const uint8_t *app_payload;
    uint16_t app_payload_len;
    uint8_t channel_hash;
    uint8_t cipher_mac[2];
    sle_team_pos_body_t pos;

    if (sle_team_decode_mesh_packet(&mesh_packet, buf, buf_len) != SLE_TEAM_OK) {
        return -1;
    }
    if (sle_team_unwrap_mesh_group_data(&mesh_packet, &channel_hash, cipher_mac, &app_payload, &app_payload_len) !=
        SLE_TEAM_OK) {
        return -2;
    }
    if (sle_team_decode_app_packet(&app_packet, app_payload, app_payload_len) != SLE_TEAM_OK) {
        return -3;
    }
    if (app_packet.app_msg_type != SLE_TEAM_APP_POS_REPORT || app_packet.body_len < sizeof(sle_team_pos_body_t)) {
        return -4;
    }

    (void)memcpy(&pos, app_packet.body, sizeof(pos));
    printf("team=%u src=%u dst=%u seq=%u lat=%ld lon=%ld battery=%u\n",
        app_packet.team_id,
        app_packet.src_id,
        app_packet.dst_id,
        app_packet.seq,
        (long)pos.latitude_e6,
        (long)pos.longitude_e6,
        pos.battery_percent);

    return 0;
}

#ifdef SLE_TEAM_PACKET_TEST
int main(void)
{
    uint8_t packet[256];
    size_t packet_len = 0;

    if (build_demo_position_packet(1, 2, 1, packet, sizeof(packet), &packet_len) != SLE_TEAM_OK) {
        return 1;
    }

    dump_hex(packet, (uint16_t)packet_len);
    return parse_demo_position_packet(packet, packet_len);
}
#endif
