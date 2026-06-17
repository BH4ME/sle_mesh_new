#include "sle_team_cli.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 通过开头宏定义切换主从角色：
 * 1 = leader
 * 0 = member
 */
#ifndef SLE_TEAM_NODE_IS_LEADER
#define SLE_TEAM_NODE_IS_LEADER 1
#endif

#ifndef SLE_TEAM_SELF_ID
#define SLE_TEAM_SELF_ID (SLE_TEAM_NODE_IS_LEADER ? 1 : 2)
#endif

#ifndef SLE_TEAM_LEADER_ID
#define SLE_TEAM_LEADER_ID 1
#endif

#ifndef SLE_TEAM_TEAM_ID
#define SLE_TEAM_TEAM_ID 1
#endif

#ifndef SLE_TEAM_CHANNEL_HASH
#define SLE_TEAM_CHANNEL_HASH 0x11
#endif

typedef struct {
    uint32_t now_s;
    uint8_t rx_inject[256];
    uint16_t rx_inject_len;
} app_runtime_t;

static app_runtime_t g_rt;
static sle_team_node_t g_node;
static sle_team_cli_t g_cli;

static int app_send(void *user_ctx, sle_team_send_kind_t kind, uint8_t dst_id, const uint8_t *buf, uint16_t len)
{
    uint16_t i;
    (void)user_ctx;

    printf("[tx] kind=%u dst=%u len=%u data=", (unsigned)kind, (unsigned)dst_id, (unsigned)len);
    for (i = 0; i < len; i++) {
        printf("%02X ", buf[i]);
    }
    printf("\n");
    return 0;
}

static uint32_t app_now(void *user_ctx)
{
    app_runtime_t *rt = (app_runtime_t *)user_ctx;
    return rt->now_s;
}

static void app_log(void *user_ctx, const char *text)
{
    (void)user_ctx;
    printf("[log] %s\n", text);
}

static void app_joined(void *user_ctx, uint8_t member_id)
{
    (void)user_ctx;
    printf("[evt] joined member=%u\n", (unsigned)member_id);
}

static void app_position(void *user_ctx, uint8_t member_id, const sle_team_pos_body_t *pos)
{
    (void)user_ctx;
    printf("[evt] pos member=%u lat=%ld lon=%ld battery=%u\n",
        (unsigned)member_id, (long)pos->latitude_e6, (long)pos->longitude_e6, (unsigned)pos->battery_percent);
}

static void app_alert(void *user_ctx, uint8_t member_id, uint8_t reason)
{
    (void)user_ctx;
    printf("[evt] alert member=%u reason=%u\n", (unsigned)member_id, (unsigned)reason);
}

static void app_cli_print(void *user_ctx, const char *text)
{
    (void)user_ctx;
    printf("[cli] %s\n", text);
}

static void app_init(void)
{
    sle_team_node_cfg_t cfg;
    sle_team_node_ops_t ops;

    memset(&cfg, 0, sizeof(cfg));
    memset(&ops, 0, sizeof(ops));

    cfg.team_id = SLE_TEAM_TEAM_ID;
    cfg.self_id = SLE_TEAM_SELF_ID;
    cfg.leader_id = SLE_TEAM_LEADER_ID;
    cfg.role = SLE_TEAM_NODE_IS_LEADER ? SLE_TEAM_ROLE_LEADER : SLE_TEAM_ROLE_MEMBER;
    cfg.channel_hash = SLE_TEAM_CHANNEL_HASH;
    cfg.report_interval_s = 5U;
    cfg.heartbeat_interval_s = 3U;
    cfg.warn_distance_m = 50U;
    cfg.lost_distance_m = 80U;
    cfg.heartbeat_timeout_s = 10U;

    ops.send = app_send;
    ops.now_s = app_now;
    ops.log = app_log;
    ops.on_joined = app_joined;
    ops.on_position = app_position;
    ops.on_alert = app_alert;
    ops.user_ctx = &g_rt;

    (void)sle_team_node_init(&g_node, &cfg, &ops);
    sle_team_cli_init(&g_cli, &g_node, app_cli_print, NULL);
}

/*
 * 你后面在 HiSpark 工程里可以这样用：
 * 1. 串口收到一行命令后调用 sle_team_cli_handle_line(...)
 * 2. SLE 收到原始包后调用 sle_team_node_on_packet(...)
 * 3. 周期定时器里调用 sle_team_node_tick(...)
 */

#ifdef SLE_TEAM_TERMINAL_TEST
int main(void)
{
    char line[192];

    app_init();
    sle_team_cli_print_help(&g_cli);

    while (fgets(line, sizeof(line), stdin) != NULL) {
        if (strncmp(line, "tick ", 5) == 0) {
            g_rt.now_s = (uint32_t)strtoul(&line[5], NULL, 0);
            sle_team_node_tick(&g_node);
            continue;
        }
        sle_team_cli_handle_line(&g_cli, line);
    }

    return 0;
}
#endif
