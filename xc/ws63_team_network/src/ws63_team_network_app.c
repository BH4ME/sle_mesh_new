#include "sle_team_cli.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "app_init.h"
#include "common_def.h"
#include "errcode.h"
#include "sle_errcode.h"
#include "nv.h"
#include "pinctrl.h"
#include "securec.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "uart.h"
#include "watchdog.h"
#if defined(CONFIG_SLE_TEAM_WIFI_AP_ENABLE)
#include "wifi_device.h"
#include "wifi_device_config.h"
#endif
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_ssap_client.h"
#include "sle_ssap_server.h"
#include "sle_team_packet.h"
#include "sle_team_relay_optimizer.h"
#include "sle_uart_client.h"
#include "sle_uart_server.h"
#include "sle_uart_server_adv.h"
#include "ws63_team_gps.h"
#include "ws63_team_http.h"
#include "ws63_team_power.h"
#include "ws63_st7789_display.h"
#include "ws63_team_status_led.h"
#ifndef CONFIG_SLE_TEAM_SELF_ID
#define CONFIG_SLE_TEAM_SELF_ID 1
#endif
#ifndef CONFIG_SLE_TEAM_LEADER_ID
#define CONFIG_SLE_TEAM_LEADER_ID 1
#endif
#ifndef CONFIG_SLE_TEAM_TEAM_ID
#define CONFIG_SLE_TEAM_TEAM_ID 1
#endif
#ifndef CONFIG_SLE_TEAM_CHANNEL_HASH
#define CONFIG_SLE_TEAM_CHANNEL_HASH 0x11
#endif
#ifndef CONFIG_SLE_TEAM_UART_BUS
#define CONFIG_SLE_TEAM_UART_BUS 0
#endif
#ifndef CONFIG_SLE_TEAM_UART_TXD_PIN
#define CONFIG_SLE_TEAM_UART_TXD_PIN 21
#endif
#ifndef CONFIG_SLE_TEAM_UART_RXD_PIN
#define CONFIG_SLE_TEAM_UART_RXD_PIN 22
#endif
#ifndef CONFIG_SLE_TEAM_HEARTBEAT_INTERVAL_S
#define CONFIG_SLE_TEAM_HEARTBEAT_INTERVAL_S 1
#endif
#ifndef CONFIG_SLE_TEAM_REPORT_INTERVAL_S
#define CONFIG_SLE_TEAM_REPORT_INTERVAL_S 5
#endif
#ifndef CONFIG_SLE_TEAM_WARN_DISTANCE_M
#define CONFIG_SLE_TEAM_WARN_DISTANCE_M 50
#endif
#ifndef CONFIG_SLE_TEAM_LOST_DISTANCE_M
#define CONFIG_SLE_TEAM_LOST_DISTANCE_M 80
#endif
#ifndef CONFIG_SLE_TEAM_HEARTBEAT_TIMEOUT_S
#define CONFIG_SLE_TEAM_HEARTBEAT_TIMEOUT_S 3
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_ENABLE
#define CONFIG_SLE_TEAM_ST7789_ENABLE 1
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_SPI_BUS
#define CONFIG_SLE_TEAM_ST7789_SPI_BUS 0
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_SCLK_PIN
#define CONFIG_SLE_TEAM_ST7789_SCLK_PIN 7
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_MOSI_PIN
#define CONFIG_SLE_TEAM_ST7789_MOSI_PIN 9
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_CS_PIN
#define CONFIG_SLE_TEAM_ST7789_CS_PIN 8
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_DC_PIN
#define CONFIG_SLE_TEAM_ST7789_DC_PIN 13
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_RESET_PIN
#define CONFIG_SLE_TEAM_ST7789_RESET_PIN 10
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_X_OFFSET
#define CONFIG_SLE_TEAM_ST7789_X_OFFSET 40
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_Y_OFFSET
#define CONFIG_SLE_TEAM_ST7789_Y_OFFSET 53
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_WIDTH
#define CONFIG_SLE_TEAM_ST7789_WIDTH 240
#endif
#ifndef CONFIG_SLE_TEAM_ST7789_HEIGHT
#define CONFIG_SLE_TEAM_ST7789_HEIGHT 135
#endif
#define SLE_TEAM_FW_VERSION "v4.5.56-minimal"
#define SLE_TEAM_FW_COMPAT 0x0556U
#define SLE_TEAM_HW_CONSTRAINTS "minimal leader/member/relay rewrite"
#define SLE_TEAM_APP_TASK_STACK_SIZE 0x1800
#define SLE_TEAM_APP_TASK_PRIO 28
#define SLE_TEAM_DISPLAY_TASK_STACK_SIZE 0x1800
#define SLE_TEAM_DISPLAY_TASK_PRIO 30
#define SLE_TEAM_REBOOT_TASK_STACK_SIZE 0x800
#define SLE_TEAM_REBOOT_TASK_PRIO 12
#define SLE_TEAM_UART_BAUDRATE 115200
#define SLE_TEAM_UART_RX_BUF_SIZE 512
#define SLE_TEAM_CLI_LINE_SIZE 192
#define SLE_TEAM_CLI_QUEUE_LEN 4
#define SLE_TEAM_CLI_QUEUE_TIMEOUT_MS 20
#define SLE_TEAM_MAIN_LOOP_SLEEP_MS 10U
#define SLE_TEAM_DISPLAY_LOOP_SLEEP_MS 25U
#define SLE_TEAM_IDENTITY_WAIT_MAX_MS 4000U
#define SLE_TEAM_LEADER_RESCAN_INTERVAL_MS 3000U
#define SLE_TEAM_DIRECT_CAP_DEFAULT 7U
#define SLE_TEAM_RELAY_CHILD_CAP_DEFAULT 7U
#define SLE_TEAM_ROUTE_ID_FALLBACK 1U
#define SLE_TEAM_ROUTE_ID_SUFFIX_HIGH_WEIGHT 31U
#define SLE_TEAM_ADV_ROUTE_HINT_LEN 6U
#define SLE_TEAM_PENDING_CONN_MAX SLE_TEAM_MAX_DIRECT_CONNECTIONS
#define SLE_TEAM_DISPLAY_LABEL_SIZE 16U
#define SLE_TEAM_NV_KEY_WEB_CONFIG 0x5001
#define SLE_TEAM_NV_KEY_ALLOWED_MEMBERS 0x5002
#define SLE_TEAM_NV_CONFIG_MAGIC 0x534C4554U
#define SLE_TEAM_NV_CONFIG_VERSION 4U
typedef struct {
    char line[SLE_TEAM_CLI_LINE_SIZE];
} sle_team_cli_msg_t;
typedef struct {
    uint8_t active;
    sle_addr_t addr;
    uint8_t route_id;
} team_pending_conn_t;

/* Display state is double-buffered with a sequence counter because the
 * networking task writes status, while TeamDisplayTask reads and paints it.
 * This keeps LVGL/ST7789 work out of the time-sensitive SLE receive path. */
typedef struct {
    volatile uint32_t seq;
    uint8_t online_count;
    uint8_t offline_count;
    uint8_t event_count;
    uint32_t event_seq;
    char role[SLE_TEAM_DISPLAY_LABEL_SIZE];
    char self[SLE_TEAM_DISPLAY_LABEL_SIZE];
} team_display_status_t;
typedef struct {
    volatile uint32_t seq;
    uint8_t event;
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint32_t last_seen_s;
    char member[SLE_TEAM_DISPLAY_LABEL_SIZE];
} team_display_event_t;
typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t role;
    uint8_t team_id;
    uint8_t channel_hash;
    uint8_t direct_cap;
    uint16_t leader_term;
    uint16_t leader_suffix;
    uint16_t checksum;
} sle_team_config_nv_t;
typedef struct {
    uint8_t uart_rx_buf[SLE_TEAM_UART_RX_BUF_SIZE];
    char line_buf[SLE_TEAM_CLI_LINE_SIZE];
    char self_label[16];
    char leader_label[16];
    char softap_ssid[32];
    uint8_t self_mac[6];
    uint8_t self_mac_ready;
    uint16_t leader_suffix;
    uint16_t line_len;
    unsigned long cli_queue_id;
    uint8_t cli_queue_ready;
    uint8_t route_id;
    uint8_t role_configured;
    uint8_t sle_started;
    uint8_t relay_client_started;
    volatile uint8_t role_request_pending;
    uint8_t role_request_role;
    uint8_t role_request_team;
    uint8_t role_request_channel;
    uint8_t role_request_leader;
    uint16_t role_request_leader_suffix;
    uint16_t role_request_leader_term;
    uint8_t role_request_direct_cap;
    uint8_t role_request_save_nv;
    int role_request_last_ret;
    uint8_t relay_scan_paused_for_upstream_loss;
    uint8_t direct_cap;
    uint8_t relay_target;
    uint8_t leader_scan_paused;
    uint32_t last_leader_rescan_ms;
    uint32_t last_relay_optimize_ms;
    int last_role_ret;
} sle_team_ws63_runtime_t;
static sle_team_ws63_runtime_t g_team_rt;
static sle_team_node_t g_team_node;
static sle_team_cli_t g_team_cli;
static sle_team_web_event_log_t g_team_events;
static sle_connection_callbacks_t g_team_conn_cbks = {0};
static team_pending_conn_t g_team_pending_conns[SLE_TEAM_PENDING_CONN_MAX];
static team_display_status_t g_team_display_status;
static team_display_event_t g_team_display_event;
static uart_buffer_config_t g_uart_buffer_config = {
    .rx_buffer = g_team_rt.uart_rx_buf,
    .rx_buffer_size = SLE_TEAM_UART_RX_BUF_SIZE,
};
static uint16_t team_self_mac_suffix(void);
static uint8_t team_route_id_from_suffix(uint16_t suffix);
static uint32_t team_now_s(void *user_ctx);
static uint8_t team_direct_cap(void);
static uint8_t team_relay_count(void);
static uint8_t team_online_count(void);
static int team_configure_role(sle_team_node_role_t role, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_term);
static int team_request_role_config(sle_team_node_role_t role, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_suffix, uint16_t leader_term, uint8_t direct_cap, uint8_t save_nv);
static void team_handle_role_request_once(void);
static void team_identity_refresh_labels(void);
static void team_identity_set_leader_suffix(uint16_t suffix);
static void team_identity_set_fallback(void);
static int team_cfg_apply_loaded(const sle_team_config_nv_t *cfg);
static int team_cfg_restore_loaded(const sle_team_config_nv_t *cfg);
static int team_cfg_status_write_json(char *out, size_t out_size);
static int team_cfg_save_leader(uint8_t team_id, uint8_t channel_hash, uint16_t leader_term, uint8_t apply_now);
static int team_cfg_save_member(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash,
    uint16_t leader_term, uint8_t apply_now);
static int team_cfg_apply_saved(void);
static int team_cfg_clear_all_saved(void);
static int team_factory_reset(void);
static void team_member_reset_after_leave(void);
static void team_reboot_schedule(const char *reason);
static void team_http_get_identity_cb(ws63_team_http_identity_t *identity);
static int team_http_pairing_start_cb(void);
static int team_http_pairing_stop_cb(void);
static int team_http_pairing_approve_cb(uint8_t member_id, uint8_t relay_allowed);
static int team_http_member_select_cb(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash);
static int team_http_member_leave_cb(void);
static const ws63_team_http_callbacks_t g_team_http_callbacks = {
    .get_identity = team_http_get_identity_cb,
    .write_config_status_json = team_cfg_status_write_json,
    .save_leader = team_cfg_save_leader,
    .save_member = team_cfg_save_member,
    .apply_saved = team_cfg_apply_saved,
    .clear_config = team_cfg_clear_all_saved,
    .reboot = team_reboot_schedule,
    .pairing_start = team_http_pairing_start_cb,
    .pairing_stop = team_http_pairing_stop_cb,
    .pairing_approve = team_http_pairing_approve_cb,
    .member_select = team_http_member_select_cb,
    .member_leave = team_http_member_leave_cb,
    .factory_reset = team_factory_reset,
};
static void team_print(const char *text)
{
    if (text != NULL) {
        osal_printk("[team] %s\r\n", text);
    }
}
static void team_cli_print(void *user_ctx, const char *text)
{
    unused(user_ctx);
    if (text != NULL) {
        osal_printk("%s\r\n", text);
    }
}
static const char *team_role_name(uint8_t role, uint8_t valid)
{
    if (valid == 0U) {
        return "none";
    }
    return role == (uint8_t)SLE_TEAM_ROLE_LEADER ? "leader" : "member";
}
static uint16_t team_nv_checksum(const sle_team_config_nv_t *cfg)
{
    const uint8_t *bytes = (const uint8_t *)cfg;
    uint16_t sum = 0x5A5AU;
    size_t i;

    if (cfg == NULL) {
        return 0U;
    }
    for (i = 0U; i < offsetof(sle_team_config_nv_t, checksum); i++) {
        sum = (uint16_t)((sum << 5) | (sum >> 11));
        sum ^= bytes[i];
    }
    return sum;
}
static uint8_t team_nv_config_valid(const sle_team_config_nv_t *cfg)
{
    if (cfg == NULL) {
        return 0U;
    }
    if (cfg->magic != SLE_TEAM_NV_CONFIG_MAGIC || cfg->version != SLE_TEAM_NV_CONFIG_VERSION) {
        return 0U;
    }
    if (cfg->role != (uint8_t)SLE_TEAM_ROLE_LEADER && cfg->role != (uint8_t)SLE_TEAM_ROLE_MEMBER) {
        return 0U;
    }
    if (cfg->team_id == 0U || cfg->team_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    if (cfg->role == (uint8_t)SLE_TEAM_ROLE_LEADER &&
        (cfg->direct_cap == 0U || cfg->direct_cap >= SLE_TEAM_MAX_DIRECT_CONNECTIONS)) {
        return 0U;
    }
    if (cfg->role == (uint8_t)SLE_TEAM_ROLE_MEMBER && cfg->leader_suffix == 0U) {
        return 0U;
    }
    return cfg->checksum == team_nv_checksum(cfg) ? 1U : 0U;
}
static int team_nv_config_save(sle_team_node_role_t role, uint8_t team_id, uint16_t leader_suffix,
    uint8_t channel_hash, uint8_t direct_cap, uint16_t leader_term)
{
    sle_team_config_nv_t cfg;
    errcode_t ret;
    errcode_t flush_ret = ERRCODE_SUCC;
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    cfg.magic = SLE_TEAM_NV_CONFIG_MAGIC;
    cfg.version = SLE_TEAM_NV_CONFIG_VERSION;
    cfg.role = (uint8_t)role;
    cfg.team_id = team_id;
    cfg.channel_hash = channel_hash;
    cfg.direct_cap = direct_cap;
    cfg.leader_term = leader_term != 0U ? leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    cfg.leader_suffix = leader_suffix;
    cfg.checksum = team_nv_checksum(&cfg);
    ret = uapi_nv_write(SLE_TEAM_NV_KEY_WEB_CONFIG, (const uint8_t *)&cfg, (uint16_t)sizeof(cfg));
    if (ret == ERRCODE_SUCC) {
        flush_ret = uapi_nv_flush();
    }
    osal_printk("[team-nv] save role=%u team=%u leader_suffix=%04X channel=%u direct_cap=%u term=%u ret=0x%x flush=0x%x\r\n",
        cfg.role, cfg.team_id, cfg.leader_suffix, cfg.channel_hash, cfg.direct_cap, cfg.leader_term, ret, flush_ret);
    return ret == ERRCODE_SUCC ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED;
}
static int team_nv_config_load(sle_team_config_nv_t *cfg)
{
    uint16_t len = 0U;
    errcode_t ret;
    if (cfg == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset_s(cfg, sizeof(*cfg), 0, sizeof(*cfg));
    ret = uapi_nv_read(SLE_TEAM_NV_KEY_WEB_CONFIG, (uint16_t)sizeof(*cfg), &len, (uint8_t *)cfg);
    if (ret != ERRCODE_SUCC || len != sizeof(*cfg) || team_nv_config_valid(cfg) == 0U) {
        return SLE_TEAM_ERR_FORMAT;
    }
    return SLE_TEAM_OK;
}
static int team_nv_config_clear(void)
{
    uint8_t blank[sizeof(sle_team_config_nv_t)];
    errcode_t ret;
    errcode_t flush_ret = ERRCODE_SUCC;
    (void)memset_s(blank, sizeof(blank), 0, sizeof(blank));
    ret = uapi_nv_write(SLE_TEAM_NV_KEY_WEB_CONFIG, blank, (uint16_t)sizeof(blank));
    if (ret == ERRCODE_SUCC) {
        flush_ret = uapi_nv_flush();
    }
    osal_printk("[team-nv] clear config ret=0x%x flush=0x%x\r\n", ret, flush_ret);
    return ret == ERRCODE_SUCC ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED;
}
static int team_nv_allowed_clear(void)
{
    uint8_t blank[SLE_TEAM_MAX_MEMBERS + 8U];
    errcode_t ret;
    errcode_t flush_ret = ERRCODE_SUCC;
    (void)memset_s(blank, sizeof(blank), 0, sizeof(blank));
    ret = uapi_nv_write(SLE_TEAM_NV_KEY_ALLOWED_MEMBERS, blank, (uint16_t)sizeof(blank));
    if (ret == ERRCODE_SUCC) {
        flush_ret = uapi_nv_flush();
    }
    osal_printk("[team-nv] clear allowed ret=0x%x flush=0x%x\r\n", ret, flush_ret);
    return ret == ERRCODE_SUCC ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED;
}
static int team_cfg_status_write_json(char *out, size_t out_size)
{
    sle_team_config_nv_t cfg;
    uint8_t nv_valid;
    uint8_t runtime_valid = g_team_rt.role_configured != 0U ? 1U : 0U;
    uint8_t runtime_role = runtime_valid != 0U ? (uint8_t)g_team_node.cfg.role : 0xFFU;
    uint8_t runtime_self = runtime_valid != 0U ? g_team_node.cfg.self_id : g_team_rt.route_id;
    int len;

    if (out == NULL || out_size == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    nv_valid = team_nv_config_load(&cfg) == SLE_TEAM_OK ? 1U : 0U;
    if (nv_valid == 0U) {
        (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
        cfg.role = 0xFFU;
    }
    len = snprintf(out, out_size,
        "{\"ok\":true,\"fw\":\"%s\",\"profile\":\"minimal\",\"selfSuffix\":\"%04X\",\"routeId\":%u,"
        "\"softapSsid\":\"%s\",\"nvValid\":%s,\"nvRole\":\"%s\",\"nvRoleValue\":%u,\"nvTeam\":%u,"
        "\"nvChannel\":%u,\"nvLeaderSuffix\":\"%04X\",\"nvLeaderTerm\":%u,\"runtimeConfigured\":%s,"
        "\"runtimeRole\":\"%s\",\"runtimeRoleValue\":%u,\"runtimeTeam\":%u,\"runtimeChannel\":%u,"
        "\"runtimeLeader\":%u,\"runtimeLeaderTerm\":%u,\"runtimeSelf\":%u,\"runtimeDirectCap\":%u,"
        "\"runtimeRelayTarget\":%u,\"runtimeRelayCount\":%u,\"runtimeOnlineCount\":%u,"
        "\"runtimeJoined\":%u,\"runtimeParent\":%u,\"runtimeRelayEnabled\":%u,"
        "\"roleRequestPending\":%s,\"roleRequestRole\":\"%s\",\"roleRequestRoleValue\":%u,"
        "\"roleRequestTeam\":%u,\"roleRequestChannel\":%u,\"roleRequestLeader\":%u,"
        "\"roleRequestLeaderSuffix\":\"%04X\",\"roleRequestLeaderTerm\":%u,"
        "\"roleRequestLastRet\":%d,\"lastRoleRet\":%d}",
        SLE_TEAM_FW_VERSION,
        team_self_mac_suffix(),
        g_team_rt.route_id,
        g_team_rt.softap_ssid[0] != '\0' ? g_team_rt.softap_ssid : "SLE",
        nv_valid != 0U ? "true" : "false",
        team_role_name(cfg.role, nv_valid),
        nv_valid != 0U ? cfg.role : 255U,
        nv_valid != 0U ? cfg.team_id : 0U,
        nv_valid != 0U ? cfg.channel_hash : 0U,
        nv_valid != 0U ? cfg.leader_suffix : 0U,
        nv_valid != 0U ? cfg.leader_term : 0U,
        runtime_valid != 0U ? "true" : "false",
        team_role_name(runtime_role, runtime_valid),
        runtime_role,
        runtime_valid != 0U ? g_team_node.cfg.team_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.channel_hash : 0U,
        runtime_valid != 0U ? g_team_node.cfg.leader_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.leader_term : 0U,
        runtime_self,
        runtime_valid != 0U && runtime_role == (uint8_t)SLE_TEAM_ROLE_LEADER ? team_direct_cap() : 0U,
        runtime_valid != 0U && runtime_role == (uint8_t)SLE_TEAM_ROLE_LEADER ? g_team_rt.relay_target : 0U,
        team_relay_count(),
        team_online_count(),
        runtime_valid != 0U ? g_team_node.joined : 0U,
        runtime_valid != 0U ? g_team_node.upstream_parent_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.relay_enabled : 0U,
        g_team_rt.role_request_pending != 0U ? "true" : "false",
        team_role_name(g_team_rt.role_request_role, g_team_rt.role_request_pending),
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_role : 255U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_team : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_channel : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader_suffix : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader_term : 0U,
        g_team_rt.role_request_last_ret,
        g_team_rt.last_role_ret);
    return (len > 0 && len < (int)out_size) ? SLE_TEAM_OK : SLE_TEAM_ERR_BUF;
}
static int team_cfg_clear_all_saved(void)
{
    int ret_cfg = team_nv_config_clear();
    int ret_allow = team_nv_allowed_clear();

    g_team_rt.role_request_pending = 0U;
    g_team_rt.role_request_role = 0U;
    g_team_rt.role_request_team = 0U;
    g_team_rt.role_request_channel = 0U;
    g_team_rt.role_request_leader = 0U;
    g_team_rt.role_request_leader_suffix = 0U;
    g_team_rt.role_request_leader_term = 0U;
    g_team_rt.role_request_direct_cap = 0U;
    g_team_rt.role_request_save_nv = 0U;
    g_team_rt.role_request_last_ret = SLE_TEAM_OK;
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        (void)sle_team_node_allow_all_members(&g_team_node);
    }
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER) {
        g_team_rt.leader_suffix = 0U;
        team_identity_refresh_labels();
    }
    return (ret_cfg == SLE_TEAM_OK && ret_allow == SLE_TEAM_OK) ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED;
}
static int team_cfg_apply_saved(void)
{
    sle_team_config_nv_t cfg;

    if (team_nv_config_load(&cfg) != SLE_TEAM_OK) {
        return SLE_TEAM_ERR_FORMAT;
    }
    return team_cfg_apply_loaded(&cfg);
}
static int team_factory_reset(void)
{
    int ret = team_cfg_clear_all_saved();

    if (ret == SLE_TEAM_OK) {
        team_identity_set_fallback();
        g_team_rt.role_configured = 0U;
        g_team_rt.sle_started = 0U;
        g_team_rt.relay_client_started = 0U;
        g_team_rt.last_role_ret = SLE_TEAM_OK;
        g_team_rt.role_request_pending = 0U;
        g_team_rt.role_request_role = 0U;
        g_team_rt.role_request_team = 0U;
        g_team_rt.role_request_channel = 0U;
        g_team_rt.role_request_leader = 0U;
        g_team_rt.role_request_leader_suffix = 0U;
        g_team_rt.role_request_leader_term = 0U;
        g_team_rt.role_request_direct_cap = 0U;
        g_team_rt.role_request_save_nv = 0U;
        g_team_rt.role_request_last_ret = SLE_TEAM_OK;
        (void)memset_s(&g_team_node, sizeof(g_team_node), 0, sizeof(g_team_node));
        (void)memset_s(&g_team_cli, sizeof(g_team_cli), 0, sizeof(g_team_cli));
        (void)memset_s(&g_team_conn_cbks, sizeof(g_team_conn_cbks), 0, sizeof(g_team_conn_cbks));
    }
    return ret;
}
static void team_member_reset_after_leave(void)
{
    team_identity_set_fallback();
    g_team_rt.role_configured = 0U;
    g_team_rt.sle_started = 0U;
    g_team_rt.relay_client_started = 0U;
    g_team_rt.last_role_ret = SLE_TEAM_OK;
    g_team_rt.role_request_pending = 0U;
    g_team_rt.role_request_role = 0U;
    g_team_rt.role_request_team = 0U;
    g_team_rt.role_request_channel = 0U;
    g_team_rt.role_request_leader = 0U;
    g_team_rt.role_request_leader_suffix = 0U;
    g_team_rt.role_request_leader_term = 0U;
    g_team_rt.role_request_direct_cap = 0U;
    g_team_rt.role_request_save_nv = 0U;
    g_team_rt.role_request_last_ret = SLE_TEAM_OK;
    g_team_rt.leader_suffix = 0U;
    (void)memset_s(&g_team_node, sizeof(g_team_node), 0, sizeof(g_team_node));
    (void)memset_s(&g_team_cli, sizeof(g_team_cli), 0, sizeof(g_team_cli));
    osal_printk("[team] member reset after leave\r\n");
}
static uint8_t team_min_u8(uint8_t left, uint8_t right) { return left < right ? left : right; }
static void team_display_label_for_member(uint8_t member_id, char *out, size_t out_len)
{
    if (out == NULL || out_len == 0U) {
        return;
    }
    if (member_id == 0U || member_id == SLE_TEAM_BROADCAST_ID) {
        (void)snprintf(out, out_len, "--");
    } else {
        (void)snprintf(out, out_len, "N%u", member_id);
    }
}
static uint8_t team_display_offline_count(void)
{
    uint8_t i;
    uint8_t count = 0U;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_node.members[i];
        if (member->member_id != 0U && member->member_id != SLE_TEAM_BROADCAST_ID &&
            member->online == 0U && (member->last_seen_s != 0U || member->position_valid != 0U)) {
            count++;
        }
    }
    return count;
}
static void team_display_publish_status(void)
{
    team_display_status_t status;
    uint32_t irq_sts;
    uint8_t changed;

    (void)memset_s(&status, sizeof(status), 0, sizeof(status));
    status.online_count = team_online_count();
    status.offline_count = team_display_offline_count();
    status.event_count = (uint8_t)(g_team_display_event.seq & 0xFFU);
    status.event_seq = g_team_display_event.seq;
    if (g_team_rt.role_configured == 0U) {
        (void)snprintf(status.role, sizeof(status.role), "idle");
    } else {
        (void)snprintf(status.role, sizeof(status.role), "%s",
            g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER ? "leader" : "member");
    }
    (void)snprintf(status.self, sizeof(status.self), "%s",
        g_team_rt.self_label[0] != '\0' ? g_team_rt.self_label : "U--");
    irq_sts = osal_irq_lock();
    changed = (uint8_t)(g_team_display_status.online_count != status.online_count ||
        g_team_display_status.offline_count != status.offline_count ||
        g_team_display_status.event_count != status.event_count ||
        g_team_display_status.event_seq != status.event_seq ||
        strcmp(g_team_display_status.role, status.role) != 0 ||
        strcmp(g_team_display_status.self, status.self) != 0);
    if (changed == 0U) {
        osal_irq_restore(irq_sts);
        return;
    }
    status.seq = g_team_display_status.seq + 1U;
    g_team_display_status = status;
    osal_irq_restore(irq_sts);
}
static void team_display_publish_event(uint8_t event, uint8_t member_id, const sle_team_pos_body_t *pos)
{
    team_display_event_t display_event;
    uint32_t irq_sts;

    (void)memset_s(&display_event, sizeof(display_event), 0, sizeof(display_event));
    display_event.event = event;
    display_event.last_seen_s = team_now_s(NULL);
    team_display_label_for_member(member_id, display_event.member, sizeof(display_event.member));
    if (pos != NULL) {
        display_event.latitude_e6 = pos->latitude_e6;
        display_event.longitude_e6 = pos->longitude_e6;
    }
    irq_sts = osal_irq_lock();
    display_event.seq = g_team_display_event.seq + 1U;
    g_team_display_event = display_event;
    osal_irq_restore(irq_sts);
    team_display_publish_status();
}
static void team_display_read_status(team_display_status_t *status)
{
    uint32_t irq_sts;

    if (status == NULL) {
        return;
    }
    irq_sts = osal_irq_lock();
    *status = g_team_display_status;
    osal_irq_restore(irq_sts);
}
static void team_display_read_event(team_display_event_t *display_event)
{
    uint32_t irq_sts;

    if (display_event == NULL) {
        return;
    }
    irq_sts = osal_irq_lock();
    *display_event = g_team_display_event;
    osal_irq_restore(irq_sts);
}
static uint8_t team_direct_cap(void)
{
    uint8_t cap = g_team_rt.direct_cap != 0U ? g_team_rt.direct_cap : SLE_TEAM_DIRECT_CAP_DEFAULT;
    if (cap >= SLE_TEAM_MAX_DIRECT_CONNECTIONS) {
        cap = (uint8_t)(SLE_TEAM_MAX_DIRECT_CONNECTIONS - 1U);
    }
    return cap == 0U ? 1U : cap;
}
static uint8_t team_physical_connect_limit(void) { return team_min_u8((uint8_t)(team_direct_cap() + 1U), (uint8_t)SLE_TEAM_MAX_DIRECT_CONNECTIONS); }
static uint8_t team_relay_count(void)
{
    uint8_t i;
    uint8_t count = 0U;
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (g_team_node.members[i].online != 0U && g_team_node.members[i].relay_allowed != 0U) {
            count++;
        }
    }
    return count;
}
static uint8_t team_online_count(void)
{
    uint8_t i;
    uint8_t count = 0U;
    if (g_team_rt.role_configured == 0U) {
        return 0U;
    }
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER) {
        return g_team_node.joined != 0U ? 1U : 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        if (g_team_node.members[i].online != 0U) {
            count++;
        }
    }
    return count;
}
static uint8_t team_leader_direct_online_count(void)
{
    uint8_t i;
    uint8_t count = 0U;
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_node.members[i];

        if ((member->online != 0U || member->policy_pending != 0U) &&
            member->parent_id == g_team_node.cfg.self_id) {
            count++;
        }
    }
    return count;
}
static uint8_t team_leader_direct_occupied_count(void)
{
    uint8_t i;
    uint8_t count = 0U;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_node.members[i];
        uint16_t conn_id = 0U;

        if (member->parent_id != g_team_node.cfg.self_id) {
            continue;
        }
        if (member->online != 0U) {
            count++;
            continue;
        }
        if (member->policy_pending != 0U &&
            sle_uart_client_find_conn_by_member(member->member_id, &conn_id) != 0U) {
            count++;
        }
    }
    return count;
}
static uint8_t team_leader_member_relay_allowed(uint8_t member_id)
{
    const sle_team_member_record_t *member = sle_team_node_find_member(&g_team_node, member_id);
    return member != NULL && member->relay_allowed != 0U ? 1U : 0U;
}
static uint8_t team_leader_member_relay_recovery_candidate(uint8_t member_id)
{
    const sle_team_member_record_t *member = sle_team_node_find_member(&g_team_node, member_id);
    return member != NULL && member->relay_recovery_candidate != 0U ? 1U : 0U;
}
static uint8_t team_leader_known_relay_child(uint8_t member_id)
{
    const sle_team_member_record_t *member;
    member = sle_team_node_find_member(&g_team_node, member_id);
    return (uint8_t)(member != NULL && member->parent_id != 0U &&
        member->parent_id != g_team_node.cfg.self_id &&
        member->parent_id != g_team_node.cfg.leader_id);
}
static uint8_t team_leader_relay_recovery_pending(void)
{
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    return g_team_node.relay_recovery_pending != 0U ? 1U : 0U;
}
static uint8_t team_leader_relay_target_pending(void)
{
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER ||
        g_team_node.cfg.pairing_enabled == 0U || g_team_rt.relay_target == 0U) {
        return 0U;
    }
    return team_relay_count() < g_team_rt.relay_target ? 1U : 0U;
}
static uint8_t team_leader_needs_auto_overflow_relay(void)
{
    uint8_t i;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    if (team_leader_direct_occupied_count() < team_direct_cap()) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_node.members[i];

        if (member->online != 0U &&
            (member->parent_id == g_team_node.cfg.self_id || member->parent_id == g_team_node.cfg.leader_id)) {
            if (member->relay_allowed == 0U) {
                return 1U;
            }
        }
    }
    return 0U;
}
static uint8_t team_leader_has_missing_allowed_member(void)
{
    uint8_t i;

    if (g_team_node.cfg.member_filter_enabled == 0U || g_team_node.cfg.allowed_member_count == 0U) {
        return 0U;
    }
    for (i = 0U; i < g_team_node.cfg.allowed_member_count; i++) {
        const sle_team_member_record_t *member = sle_team_node_find_member(&g_team_node,
            g_team_node.cfg.allowed_member_ids[i]);

        if (member == NULL || member->online == 0U) {
            return 1U;
        }
    }
    return 0U;
}
static uint8_t team_leader_should_connect_candidate(uint8_t route_id)
{
    uint8_t direct_full;
    uint8_t relay_allowed;
    uint8_t relay_target_pending;
    uint8_t auto_overflow_relay_needed;

    if (g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 1U;
    }
    if (team_leader_relay_recovery_pending() != 0U) {
        if (g_team_node.relay_recovery_selected_id != 0U) {
            if (route_id != g_team_node.relay_recovery_selected_id) {
                return 0U;
            }
            return 1U;
        }
        if (team_leader_member_relay_recovery_candidate(route_id) == 0U) {
            return 0U;
        }
        return 1U;
    }
    if (team_leader_known_relay_child(route_id) != 0U) { return 0U; }
    relay_allowed = team_leader_member_relay_allowed(route_id);
    direct_full = team_leader_direct_occupied_count() >= team_direct_cap() ? 1U : 0U;
    relay_target_pending = team_leader_relay_target_pending();
    auto_overflow_relay_needed = team_leader_needs_auto_overflow_relay();
    if (relay_target_pending != 0U && direct_full != 0U && relay_allowed == 0U) {
        return 0U;
    }
    if (direct_full != 0U && relay_allowed == 0U && auto_overflow_relay_needed == 0U) {
        return 0U;
    }
    return 1U;
}
static uint8_t team_leader_needs_scan(void)
{
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    if (team_leader_relay_recovery_pending() != 0U) {
        return 1U;
    }
    if (g_team_node.cfg.pairing_enabled != 0U) {
        return 1U;
    }
    if (team_leader_has_missing_allowed_member() != 0U) {
        return 1U;
    }
    if (team_leader_direct_occupied_count() < team_direct_cap()) {
        return 1U;
    }
    if (team_leader_needs_auto_overflow_relay() != 0U) {
        return 1U;
    }
    if (g_team_rt.relay_target != 0U && team_relay_count() < g_team_rt.relay_target) {
        return 1U;
    }
    return 0U;
}
static void team_leader_update_scan_policy(void)
{
    uint32_t now_ms;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return;
    }
    if (team_leader_needs_scan() == 0U) {
        if (g_team_rt.leader_scan_paused == 0U) {
            sle_uart_client_pause_scan("leader-capacity-satisfied");
            g_team_rt.leader_scan_paused = 1U;
        }
        return;
    }
    if (g_team_rt.leader_scan_paused != 0U) {
        sle_uart_client_resume_scan("leader-needs-candidate");
        g_team_rt.leader_scan_paused = 0U;
    }
    now_ms = (uint32_t)uapi_tcxo_get_ms();
    if ((uint32_t)(now_ms - g_team_rt.last_leader_rescan_ms) < SLE_TEAM_LEADER_RESCAN_INTERVAL_MS) {
        return;
    }
    g_team_rt.last_leader_rescan_ms = now_ms;
    sle_uart_client_force_rescan();
}
static uint8_t team_route_id_from_suffix(uint16_t suffix)
{
    uint16_t mix;

    if (suffix == 0U) {
        return SLE_TEAM_ROUTE_ID_FALLBACK;
    }
    mix = (uint16_t)((suffix & 0xFFU) +
        (((suffix >> 8U) & 0xFFU) * SLE_TEAM_ROUTE_ID_SUFFIX_HIGH_WEIGHT));
    return (uint8_t)((mix % 254U) + 1U);
}
static uint16_t team_self_mac_suffix(void)
{
    if (g_team_rt.self_mac_ready == 0U) {
        return (uint16_t)g_team_rt.route_id;
    }
    return (uint16_t)(((uint16_t)g_team_rt.self_mac[4] << 8U) | g_team_rt.self_mac[5]);
}
static uint16_t team_active_leader_suffix(void)
{
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        return team_self_mac_suffix();
    }
    return g_team_rt.leader_suffix;
}
static uint8_t team_route_id_from_mac(const uint8_t mac[6])
{
    uint16_t suffix;

    if (mac == NULL) {
        return (uint8_t)CONFIG_SLE_TEAM_SELF_ID;
    }
    suffix = (uint16_t)(((uint16_t)mac[4] << 8U) | mac[5]);
    return team_route_id_from_suffix(suffix);
}
static uint8_t team_leader_member_matches_disconnect_addr(const sle_team_member_record_t *member, const sle_addr_t *addr)
{
    if (member == NULL || addr == NULL || member->mac_ready == 0U) {
        return 0U;
    }
    return (uint8_t)(member->mac[4] == addr->addr[4] && member->mac[5] == addr->addr[5]);
}
static uint8_t team_leader_resolve_disconnected_member(uint16_t conn_id, const sle_addr_t *addr)
{
    uint8_t member_id = 0U;
    uint8_t route_id = 0U;
    uint8_t i;
    const sle_team_member_record_t *member;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    if (sle_uart_client_get_conn_member(conn_id, &member_id) != 0U && member_id != 0U) {
        return member_id;
    }
    if (addr == NULL) {
        return 0U;
    }
    route_id = team_route_id_from_mac(addr->addr);
    member = sle_team_node_find_member(&g_team_node, route_id);
    if (member != NULL &&
        (member->parent_id == g_team_node.cfg.self_id || member->parent_id == g_team_node.cfg.leader_id) &&
        (member->mac_ready == 0U || team_leader_member_matches_disconnect_addr(member, addr) != 0U)) {
        return member->member_id;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        member = &g_team_node.members[i];
        if (member->member_id == 0U ||
            (member->online == 0U && member->policy_pending == 0U) ||
            (member->parent_id != g_team_node.cfg.self_id && member->parent_id != g_team_node.cfg.leader_id)) {
            continue;
        }
        if (team_leader_member_matches_disconnect_addr(member, addr) != 0U) {
            return member->member_id;
        }
    }
    return 0U;
}
static void team_identity_refresh_labels(void)
{
    uint16_t leader_suffix;

    if (g_team_rt.self_mac_ready != 0U) {
        (void)snprintf(g_team_rt.self_label, sizeof(g_team_rt.self_label), "SLE-%02X%02X",
            g_team_rt.self_mac[4], g_team_rt.self_mac[5]);
    } else {
        (void)snprintf(g_team_rt.self_label, sizeof(g_team_rt.self_label), "SLE-%02X",
            g_team_rt.route_id);
    }
    if (g_team_rt.self_mac_ready != 0U) {
        (void)snprintf(g_team_rt.softap_ssid, sizeof(g_team_rt.softap_ssid), "SLE-%02X%02X",
            g_team_rt.self_mac[4], g_team_rt.self_mac[5]);
    } else {
        (void)snprintf(g_team_rt.softap_ssid, sizeof(g_team_rt.softap_ssid), "SLE-%02X",
            g_team_rt.route_id);
    }
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        (void)snprintf(g_team_rt.leader_label, sizeof(g_team_rt.leader_label), "%s", g_team_rt.self_label);
        g_team_rt.leader_suffix = team_self_mac_suffix();
    } else {
        leader_suffix = team_active_leader_suffix();
        if (leader_suffix != 0U) {
            (void)snprintf(g_team_rt.leader_label, sizeof(g_team_rt.leader_label), "L%04X", leader_suffix);
        } else {
            (void)snprintf(g_team_rt.leader_label, sizeof(g_team_rt.leader_label), "L%02X",
                g_team_node.cfg.leader_id != 0U ? g_team_node.cfg.leader_id : CONFIG_SLE_TEAM_LEADER_ID);
        }
    }
}
static void team_identity_set_leader_suffix(uint16_t suffix)
{
    g_team_rt.leader_suffix = suffix;
    if (suffix != 0U) {
        (void)snprintf(g_team_rt.leader_label, sizeof(g_team_rt.leader_label), "L%04X", suffix);
    } else {
        team_identity_refresh_labels();
    }
}
static void team_identity_set_fallback(void)
{
    g_team_rt.route_id = (uint8_t)CONFIG_SLE_TEAM_SELF_ID;
    g_team_rt.self_mac_ready = 0U;
    g_team_rt.leader_suffix = 0U;
    (void)memset_s(g_team_rt.self_mac, sizeof(g_team_rt.self_mac), 0, sizeof(g_team_rt.self_mac));
    team_identity_refresh_labels();
}
static void team_identity_read_mac(void)
{
#if defined(CONFIG_SLE_TEAM_WIFI_AP_ENABLE)
    int8_t mac[6] = {0};
    errcode_t ret;
    uint32_t waited_ms = 0U;

    if (wifi_is_wifi_inited() == 0) {
        ret = wifi_init();
        osal_printk("[team-id] wifi_init ret=0x%x\r\n", ret);
    }
    while (wifi_is_wifi_inited() == 0 && waited_ms < SLE_TEAM_IDENTITY_WAIT_MAX_MS) {
        osal_msleep(100);
        waited_ms += 100U;
    }
    if (wifi_is_wifi_inited() == 0) {
        osal_printk("[team-id] wifi init timeout fallback route=%u\r\n", g_team_rt.route_id);
        return;
    }
    ret = wifi_get_base_mac_addr(mac, sizeof(mac));
    if (ret != ERRCODE_SUCC) {
        osal_printk("[team-id] mac read failed ret=0x%x fallback route=%u\r\n", ret, g_team_rt.route_id);
        return;
    }
    (void)memcpy_s(g_team_rt.self_mac, sizeof(g_team_rt.self_mac), mac, sizeof(g_team_rt.self_mac));
    g_team_rt.self_mac_ready = 1U;
    g_team_rt.route_id = team_route_id_from_mac(g_team_rt.self_mac);
    team_identity_refresh_labels();
    osal_printk("[team-id] label=%s route=%u mac=%02X:%02X:%02X:%02X:%02X:%02X\r\n",
        g_team_rt.self_label, g_team_rt.route_id,
        g_team_rt.self_mac[0], g_team_rt.self_mac[1], g_team_rt.self_mac[2],
        g_team_rt.self_mac[3], g_team_rt.self_mac[4], g_team_rt.self_mac[5]);
#endif
}
static void team_identity_init(void)
{
    team_identity_set_fallback();
    team_identity_read_mac();
}
static void team_http_get_identity_cb(ws63_team_http_identity_t *identity)
{
    if (identity == NULL) {
        return;
    }
    (void)memset_s(identity, sizeof(*identity), 0, sizeof(*identity));
    (void)snprintf(identity->self_label, sizeof(identity->self_label), "%s", g_team_rt.self_label);
    (void)snprintf(identity->leader_label, sizeof(identity->leader_label), "%s", g_team_rt.leader_label);
    (void)snprintf(identity->softap_ssid, sizeof(identity->softap_ssid), "%s", g_team_rt.softap_ssid);
    if (g_team_rt.self_mac_ready != 0U) {
        (void)memcpy_s(identity->self_mac, sizeof(identity->self_mac),
            g_team_rt.self_mac, sizeof(g_team_rt.self_mac));
    }
    identity->self_mac_ready = g_team_rt.self_mac_ready;
    identity->route_id = g_team_rt.route_id;
    identity->self_suffix = team_self_mac_suffix();
    identity->role_configured = g_team_rt.role_configured;
    identity->role_request_pending = g_team_rt.role_request_pending;
    identity->role_request_role = g_team_rt.role_request_role;
    identity->role_request_team = g_team_rt.role_request_team;
    identity->role_request_channel = g_team_rt.role_request_channel;
    identity->role_request_leader = g_team_rt.role_request_leader;
    identity->role_request_leader_suffix = g_team_rt.role_request_leader_suffix;
    identity->role_request_leader_term = g_team_rt.role_request_leader_term;
    identity->role_request_last_ret = g_team_rt.role_request_last_ret;
    identity->fw_version = SLE_TEAM_FW_VERSION;
}
static int team_http_pairing_start_cb(void)
{
    return sle_team_node_pairing_start(&g_team_node);
}
static int team_http_pairing_stop_cb(void)
{
    return sle_team_node_pairing_stop(&g_team_node);
}
static int team_http_pairing_approve_cb(uint8_t member_id, uint8_t relay_allowed)
{
    return sle_team_node_pairing_approve_with_relay(&g_team_node, member_id, relay_allowed);
}
static int team_http_member_select_cb(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash)
{
    return team_cfg_save_member(leader_suffix, team_id, channel_hash, SLE_TEAM_LEADER_TERM_DEFAULT, 1U);
}
static int team_http_member_leave_cb(void)
{
    int ret;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return SLE_TEAM_ERR_ARG;
    }
    ret = sle_team_node_member_leave(&g_team_node);
    if (ret == SLE_TEAM_OK) {
        (void)team_nv_config_clear();
        team_member_reset_after_leave();
    }
    return ret;
}
static void team_sle_prepare_local_addr(void)
{
    uint8_t sle_addr[6] = {0x02, 0x53, 0x4C, 0x45, 0x00, 0x01};

    if (g_team_rt.self_mac_ready != 0U) {
        (void)memcpy_s(sle_addr, sizeof(sle_addr), g_team_rt.self_mac, sizeof(g_team_rt.self_mac));
        sle_addr[0] = (uint8_t)((sle_addr[0] | 0x02U) & 0xFEU);
    } else {
        sle_addr[4] = 0U;
        sle_addr[5] = g_team_rt.route_id;
    }
    sle_uart_server_adv_set_local_addr(sle_addr);
    osal_printk("[team] sle local addr=%02X:%02X:%02X:%02X:%02X:%02X route=%u\r\n",
        sle_addr[0], sle_addr[1], sle_addr[2], sle_addr[3], sle_addr[4], sle_addr[5],
        g_team_rt.route_id);
}
static uint32_t team_now_s(void *user_ctx)
{
    unused(user_ctx);
    return (uint32_t)(uapi_tcxo_get_ms() / 1000U);
}
static int8_t team_rssi_dbm(void *user_ctx)
{
    unused(user_ctx);
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        return sle_uart_client_get_last_rssi();
    }
    return sle_uart_server_get_last_rssi();
}
static uint8_t team_battery_percent(void *user_ctx)
{
    unused(user_ctx);
    return ws63_team_power_battery_percent();
}
static void team_node_log(void *user_ctx, const char *text)
{
    unused(user_ctx);
    team_print(text);
}
static void team_status_led_update(void)
{
    /* Only one visual state is selected at a time. Order matters: leader and
     * joining/lost states should not be blended with member/relay colors. */
    if (g_team_rt.role_configured == 0U) { ws63_team_status_led_idle(); return; }
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) { ws63_team_status_led_leader(); return; }
    if (g_team_node.joined == 0U) { ws63_team_status_led_joining(); return; }
    if (g_team_node.cfg.relay_enabled != 0U || g_team_node.cfg.relay_allowed != 0U) { ws63_team_status_led_relay(); return; }
    if (g_team_node.upstream_parent_id != 0U && g_team_node.upstream_parent_id != g_team_node.cfg.leader_id) { ws63_team_status_led_child(); return; }
    ws63_team_status_led_member();
}
static void team_on_joined(void *user_ctx, uint8_t member_id)
{
    int ret;

    unused(user_ctx);
    /* The core accepts the join; the WS63 shell decides whether the newly
     * direct member should immediately become a relay to satisfy relay_target. */
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER && g_team_rt.relay_target != 0U &&
        team_relay_count() < g_team_rt.relay_target) {
        ret = sle_team_node_grant_relay(&g_team_node, member_id);
        osal_printk("[team] relay grant member=%u target=%u relays=%u ret=%d\r\n",
            member_id, g_team_rt.relay_target, team_relay_count(), ret);
    }
    osal_printk("[team] joined member=%u online=%u relays=%u\r\n",
        member_id, team_online_count(), team_relay_count());
    team_display_publish_event(WS63_ST7789_EVENT_JOIN, member_id, NULL);
    team_status_led_update();
}
static void team_leader_fill_relay_target(void)
{
    uint8_t i;

    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER ||
        g_team_rt.relay_target == 0U) {
        return;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS && team_relay_count() < g_team_rt.relay_target; i++) {
        sle_team_member_record_t *member = &g_team_node.members[i];

        if (member->online == 0U || member->relay_allowed != 0U || member->member_id == 0U ||
            member->member_id == SLE_TEAM_BROADCAST_ID) {
            continue;
        }
        if (member->parent_id != g_team_node.cfg.self_id && member->parent_id != g_team_node.cfg.leader_id) {
            continue;
        }
        (void)sle_team_node_grant_relay(&g_team_node, member->member_id);
    }
}
static void team_on_position(void *user_ctx, uint8_t member_id, const sle_team_pos_body_t *pos)
{
    unused(user_ctx);
    if (pos != NULL) {
        osal_printk("[team] pos member=%u lat=%ld lon=%ld fix=%u\r\n",
            member_id, (long)pos->latitude_e6, (long)pos->longitude_e6, pos->fix_status);
        /* Position packets are steady telemetry, not link events. Publishing
         * them as REJOIN made the display flash a bright green event card for
         * every GPS/location update after enrollment. */
    }
}
static void team_on_alert(void *user_ctx, uint8_t member_id, uint8_t reason)
{
    unused(user_ctx);
    osal_printk("[team] alert member=%u reason=%u\r\n", member_id, reason);
    if (reason == (uint8_t)SLE_TEAM_ALERT_LEAVE) {
        osal_printk("[team] member offline id=%u reason=leave\r\n", member_id);
        team_display_publish_event(WS63_ST7789_EVENT_LEFT, member_id, NULL);
    } else {
        team_display_publish_event(WS63_ST7789_EVENT_LOST, member_id, NULL);
    }
}
static void team_on_relay_offline(void *user_ctx, uint8_t member_id)
{
    unused(user_ctx);
    osal_printk("[team] relay offline member=%u\r\n", member_id);
    team_display_publish_event(WS63_ST7789_EVENT_LOST, member_id, NULL);
    ws63_team_status_led_lost();
    /* Relay loss is handled by the portable core, but the radio scanner lives
     * in this WS63 layer. Kick scanning here so a replacement relay can be
     * found without waiting for the next slow rescan window. */
    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        sle_uart_client_resume_scan("relay-offline-recovery");
        g_team_rt.leader_scan_paused = 0U;
        sle_uart_client_force_rescan();
    }
}
static uint8_t team_member_timeout_defer(void *user_ctx, uint8_t member_id, uint32_t now_s, uint32_t last_seen_s)
{
    unused(user_ctx);
    unused(member_id);
    unused(now_s);
    unused(last_seen_s);
    return 0U;
}
static uint8_t team_buffer_contains(const uint8_t *buf, size_t buf_len, const char *needle, size_t needle_len)
{
    size_t i;

    if (buf == NULL || needle == NULL || needle_len == 0U || buf_len < needle_len) {
        return 0U;
    }
    for (i = 0U; i + needle_len <= buf_len; i++) {
        if (memcmp(buf + i, needle, needle_len) == 0) {
            return 1U;
        }
    }
    return 0U;
}
static uint8_t team_route_id_from_adv_data(const uint8_t *data, uint16_t len)
{
    return sle_team_scan_route_id_from_data(data, len);
}
static uint8_t team_route_id_from_sle_addr(const uint8_t addr[6])
{
    uint16_t suffix;

    if (addr == NULL) {
        return 0U;
    }
    suffix = (uint16_t)(((uint16_t)addr[4] << 8U) | addr[5]);
    return suffix != 0U ? team_route_id_from_suffix(suffix) : 0U;
}
static uint8_t team_addr_equal(const sle_addr_t *left, const sle_addr_t *right)
{
    if (left == NULL || right == NULL) {
        return 0U;
    }
    return memcmp(left->addr, right->addr, sizeof(left->addr)) == 0 ? 1U : 0U;
}
static void team_pending_note(const sle_addr_t *addr, uint8_t route_id)
{
    uint8_t i;
    team_pending_conn_t *free_slot = NULL;

    /* Advertisement data is seen before the SLE connection callback provides a
     * conn_id. Keep the route id by address so pair-complete can bind it later. */
    if (addr == NULL || route_id == 0U || route_id == SLE_TEAM_BROADCAST_ID) {
        return;
    }
    for (i = 0U; i < SLE_TEAM_PENDING_CONN_MAX; i++) {
        if (g_team_pending_conns[i].active != 0U && team_addr_equal(&g_team_pending_conns[i].addr, addr) != 0U) {
            g_team_pending_conns[i].route_id = route_id;
            return;
        }
        if (free_slot == NULL && g_team_pending_conns[i].active == 0U) {
            free_slot = &g_team_pending_conns[i];
        }
    }
    if (free_slot != NULL) {
        (void)memset_s(free_slot, sizeof(*free_slot), 0, sizeof(*free_slot));
        free_slot->active = 1U;
        free_slot->addr = *addr;
        free_slot->route_id = route_id;
    }
}
static uint8_t team_pending_take(const sle_addr_t *addr, uint8_t *route_id)
{
    uint8_t i;

    if (addr == NULL || route_id == NULL) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_PENDING_CONN_MAX; i++) {
        if (g_team_pending_conns[i].active != 0U && team_addr_equal(&g_team_pending_conns[i].addr, addr) != 0U) {
            *route_id = g_team_pending_conns[i].route_id;
            (void)memset_s(&g_team_pending_conns[i], sizeof(g_team_pending_conns[i]), 0,
                sizeof(g_team_pending_conns[i]));
            return 1U;
        }
    }
    return 0U;
}
static uint8_t team_decode_app_packet_from_buf(const uint8_t *buf, uint16_t len, sle_team_app_packet_t *app_packet)
{
    sle_team_mesh_packet_t mesh_packet;
    const uint8_t *app_payload = NULL;
    uint16_t app_payload_len = 0U;
    uint8_t channel_hash = 0U;
    uint8_t cipher_mac[2];

    if (buf == NULL || app_packet == NULL || g_team_rt.role_configured == 0U) {
        return 0U;
    }
    if (sle_team_decode_mesh_packet(&mesh_packet, buf, len) != SLE_TEAM_OK) {
        return 0U;
    }
    if (sle_team_unwrap_mesh_group_data(&mesh_packet, &channel_hash, cipher_mac, &app_payload, &app_payload_len) !=
        SLE_TEAM_OK) {
        return 0U;
    }
    if (channel_hash != g_team_node.cfg.channel_hash) {
        return 0U;
    }
    if (sle_team_decode_app_packet(app_packet, app_payload, app_payload_len) != SLE_TEAM_OK) {
        return 0U;
    }
    return 1U;
}
static void team_bind_packet_source(uint16_t conn_id, uint8_t from_client, const sle_team_app_packet_t *app)
{
    const sle_team_member_record_t *member = NULL;
    uint8_t bound_id = 0U;
    uint8_t direct_parent;
    uint8_t i;

    if (app == NULL || app->src_id == 0U || app->src_id == SLE_TEAM_BROADCAST_ID) {
        return;
    }
    /* from_client means the local SLE client received data from a peer it
     * dialed. Leaders use that side for direct members and relays use it for
     * children, so binding must be careful around forwarded child packets. */
    if (from_client != 0U) {
        if (g_team_rt.role_configured != 0U &&
            g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER &&
            sle_uart_client_get_conn_member(conn_id, &bound_id) != 0U &&
            bound_id != 0U &&
            bound_id != app->src_id &&
            bound_id != g_team_node.cfg.self_id &&
            bound_id != g_team_node.cfg.leader_id) {
            for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
                sle_team_member_record_t *pending_member = &g_team_node.members[i];
                if (pending_member->member_id == app->src_id &&
                    (pending_member->policy_pending != 0U ||
                    pending_member->relay_recovery_candidate != 0U)) {
                    direct_parent = (uint8_t)(pending_member->parent_id == g_team_node.cfg.self_id ||
                        pending_member->parent_id == g_team_node.cfg.leader_id);
                    if (pending_member->parent_id == bound_id ||
                        (direct_parent != 0U &&
                        g_team_node.relay_recovery_selected_id == pending_member->member_id)) {
                        pending_member->next_hop_id = bound_id;
                    }
                    break;
                }
            }
        }
        member = sle_team_node_find_member(&g_team_node, app->src_id);
        if (sle_uart_client_get_conn_member(conn_id, &bound_id) == 0U || bound_id == 0U) {
            if (member != NULL && member->parent_id != 0U &&
                member->parent_id != g_team_node.cfg.self_id &&
                member->parent_id != g_team_node.cfg.leader_id) {
                return;
            }
            (void)sle_uart_client_bind_member_conn(app->src_id, conn_id);
        }
        return;
    }
    if (sle_uart_server_get_conn_member(conn_id, &bound_id) == 0U || bound_id == 0U) {
        (void)sle_uart_server_bind_member_conn(app->src_id, conn_id);
    }
}
static uint8_t team_leader_ingress_relay_id(uint16_t conn_id, uint8_t from_client, const sle_team_app_packet_t *app)
{
    uint8_t bound_id = 0U;
    if (from_client == 0U || app == NULL || g_team_rt.role_configured == 0U ||
        g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER || app->src_id == 0U ||
        app->src_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    if (sle_uart_client_get_conn_member(conn_id, &bound_id) == 0U || bound_id == 0U ||
        bound_id == app->src_id || bound_id == g_team_node.cfg.self_id ||
        bound_id == g_team_node.cfg.leader_id) {
        return 0U;
    }
    return bound_id;
}
static uint8_t team_leader_drop_stale_direct_conn(uint16_t conn_id, uint8_t from_client, const sle_team_app_packet_t *app)
{
    const sle_team_member_record_t *member;
    uint8_t bound_id = 0U;
    if (from_client == 0U || app == NULL || g_team_rt.role_configured == 0U ||
        g_team_node.cfg.role != SLE_TEAM_ROLE_LEADER ||
        app->src_id == 0U || app->src_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    member = sle_team_node_find_member(&g_team_node, app->src_id);
    if (member == NULL || member->parent_id == 0U ||
        member->parent_id == g_team_node.cfg.self_id ||
        member->parent_id == g_team_node.cfg.leader_id) {
        return 0U;
    }
    if (g_team_node.relay_recovery_pending != 0U && g_team_node.relay_recovery_selected_id == app->src_id) {
        return 0U;
    }
    if (sle_uart_client_get_conn_member(conn_id, &bound_id) == 0U || bound_id != app->src_id) {
        return 0U;
    }
    /* Once a member has been moved under a relay, its old direct physical link
     * can still deliver a few packets. Dropping that stale direct link prevents
     * the leader from treating the old path as the current topology. */
    if (member->policy_pending != 0U) {
        if (g_team_node.relay_recovery_pending != 0U &&
            app->src_id != g_team_node.relay_recovery_lost_relay_id) {
            osal_printk("[team] drop pending stale direct conn=%u member=%u new_parent=%u\r\n",
                conn_id, app->src_id, member->parent_id);
            (void)sle_uart_client_disconnect_conn(conn_id);
            return 1U;
        } else {
            osal_printk("[team] keep pending direct conn=%u member=%u new_parent=%u\r\n",
                conn_id, app->src_id, member->parent_id);
        }
        return 0U;
    }
    osal_printk("[team] drop stale direct conn=%u member=%u new_parent=%u\r\n",
        conn_id, app->src_id, member->parent_id);
    (void)sle_uart_client_disconnect_conn(conn_id);
    return 1U;
}
static uint16_t team_fw_compat_from_adv_data(const uint8_t *data, uint16_t len)
{
    return sle_team_scan_fw_compat_from_data(data, len);
}
static uint8_t team_leader_target_conn_for_member(uint8_t dst_id, uint16_t *conn_id)
{
    const sle_team_member_record_t *member;
    uint8_t physical_id = dst_id;
    if (conn_id == NULL || dst_id == 0U || dst_id == SLE_TEAM_BROADCAST_ID) {
        return 0U;
    }
    member = sle_team_node_find_member(&g_team_node, dst_id);
    /* Logical destination may be a child behind a relay. Convert the logical
     * member id to the physical next hop before using the WS63 client table. */
    if (member != NULL) {
        if (member->policy_pending != 0U &&
            (g_team_node.relay_recovery_pending == 0U ||
            member->parent_id == g_team_node.cfg.self_id ||
            member->parent_id == g_team_node.cfg.leader_id ||
            member->member_id == g_team_node.relay_recovery_lost_relay_id) &&
            sle_uart_client_find_conn_by_member(member->member_id, conn_id) != 0U) {
            return 1U;
        }
        if (member->next_hop_id != 0U &&
            member->next_hop_id != g_team_node.cfg.self_id &&
            member->next_hop_id != g_team_node.cfg.leader_id) {
            physical_id = member->next_hop_id;
        } else if (member->parent_id != 0U &&
            member->parent_id != g_team_node.cfg.self_id &&
            member->parent_id != g_team_node.cfg.leader_id) {
            physical_id = member->parent_id;
        }
    }
    return sle_uart_client_find_conn_by_member(physical_id, conn_id);
}
static int team_sle_send(void *user_ctx, sle_team_send_kind_t kind, uint8_t dst_id, const uint8_t *buf, uint16_t len)
{
    uint16_t server_conn_id = 0U;
    uint16_t conn_id = 0U;
    errcode_t ret = ERRCODE_SLE_SUCCESS;

    unused(user_ctx);
    unused(kind);
    if (buf == NULL || len == 0U || g_team_rt.role_configured == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    /* This is the hardware adapter for the portable node core:
     * leader -> SLE client links, member upstream -> SLE server link,
     * relay downstream -> SLE client links. */
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        if (dst_id == SLE_TEAM_BROADCAST_ID) {
            ret = sle_uart_client_send_all(buf, len);
        } else if (team_leader_target_conn_for_member(dst_id, &conn_id) != 0U) {
            ret = sle_uart_client_send_by_conn(conn_id, buf, len);
        } else {
            osal_printk("[sle-tx-fail] role=leader dst=%u reason=no-route\r\n", dst_id);
            return SLE_TEAM_ERR_UNSUPPORTED;
        }
        return ret == ERRCODE_SLE_SUCCESS ? SLE_TEAM_OK : SLE_TEAM_ERR_FORMAT;
    }
    if (dst_id == g_team_node.cfg.leader_id || dst_id == SLE_TEAM_BROADCAST_ID) {
        if (sle_uart_server_connected_count() == 0U) {
            return SLE_TEAM_ERR_UNSUPPORTED;
        }
        if (dst_id == g_team_node.cfg.leader_id &&
            g_team_node.upstream_parent_id != 0U &&
            sle_uart_server_find_conn_by_member_ex(g_team_node.upstream_parent_id, &server_conn_id) != 0U) {
            ret = sle_uart_server_send_report_by_conn(server_conn_id, buf, len);
        } else {
            ret = sle_uart_server_send_report_by_handle(buf, len);
        }
        return ret == ERRCODE_SLE_SUCCESS ? SLE_TEAM_OK : SLE_TEAM_ERR_FORMAT;
    }
    if (g_team_node.cfg.relay_enabled != 0U && g_team_rt.relay_client_started != 0U &&
        sle_uart_client_find_conn_by_member(dst_id, &conn_id) != 0U) {
        ret = sle_uart_client_send_by_conn(conn_id, buf, len);
        return ret == ERRCODE_SLE_SUCCESS ? SLE_TEAM_OK : SLE_TEAM_ERR_FORMAT;
    }
    return SLE_TEAM_ERR_UNSUPPORTED;
}
static int team_handle_received_packet(uint16_t conn_id, uint8_t from_client, const uint8_t *buf, uint16_t len)
{
    sle_team_app_packet_t app_packet;
    uint8_t decoded = 0U;
    uint8_t old_upstream_parent_id = 0U;
    uint8_t ingress_relay_id = 0U;
    int ret;

    if (buf == NULL || len == 0U || g_team_rt.role_configured == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER) {
        old_upstream_parent_id = g_team_node.upstream_parent_id;
    }
    decoded = team_decode_app_packet_from_buf(buf, len, &app_packet);
    if (decoded != 0U) {
        /* Decode once in the WS63 shell so physical connection bookkeeping can
         * happen before the protocol core updates logical topology. */
        team_bind_packet_source(conn_id, from_client, &app_packet);
        if (team_leader_drop_stale_direct_conn(conn_id, from_client, &app_packet) != 0U) {
            return SLE_TEAM_OK;
        }
        ingress_relay_id = team_leader_ingress_relay_id(conn_id, from_client, &app_packet);
        g_team_node.rx_ingress_relay_id = ingress_relay_id;
    }
    ret = sle_team_node_on_packet(&g_team_node, buf, len);
    g_team_node.rx_ingress_relay_id = 0U;
    osal_printk("[team-rx] conn=%u side=%s ret=%d\r\n", conn_id, from_client != 0U ? "client" : "server", ret);
    if (decoded != 0U) {
        if (from_client != 0U &&
            g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER &&
            app_packet.app_msg_type == SLE_TEAM_APP_HELLO &&
            (ret == SLE_TEAM_ERR_UNSUPPORTED ||
            (ret == SLE_TEAM_ERR_FORMAT && app_packet.body_len < sizeof(sle_team_hello_body_t)))) {
            /* A bad forwarded child HELLO must not drop the relay's physical
             * connection; only direct rejected HELLOs are disconnected here. */
            if (ingress_relay_id != 0U) {
                osal_printk("[team] keep relay conn=%u member=%u rejected forwarded hello src=%u\r\n",
                    conn_id, ingress_relay_id, app_packet.src_id);
                return SLE_TEAM_OK;
            }
            osal_printk("[team] drop rejected hello conn=%u src=%u team=%u fw_gate=0x%x\r\n",
                conn_id, app_packet.src_id, app_packet.team_id, (uint16_t)SLE_TEAM_FW_COMPAT);
            (void)sle_uart_client_disconnect_conn(conn_id);
            return SLE_TEAM_OK;
        }
        if (from_client == 0U &&
            g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER &&
            app_packet.app_msg_type == SLE_TEAM_APP_ROUTE_UPDATE &&
            old_upstream_parent_id != 0U &&
            g_team_node.upstream_parent_id != 0U &&
            g_team_node.upstream_parent_id != old_upstream_parent_id) {
            osal_printk("[team] drop stale upstream conn=%u old_parent=%u new_parent=%u\r\n",
                conn_id, old_upstream_parent_id, g_team_node.upstream_parent_id);
            (void)sle_uart_server_disconnect_conn(conn_id);
        }
        (void)team_leader_drop_stale_direct_conn(conn_id, from_client, &app_packet);
    }
    return ret;
}
static void team_server_read_cb(uint8_t server_id, uint16_t conn_id, ssaps_req_read_cb_t *read_cb_para,
    errcode_t status)
{
    unused(server_id);
    unused(conn_id);
    unused(read_cb_para);
    unused(status);
}
static void team_server_write_cb(uint8_t server_id, uint16_t conn_id, ssaps_req_write_cb_t *write_cb_para,
    errcode_t status)
{
    unused(server_id);
    if (status != ERRCODE_SLE_SUCCESS || write_cb_para == NULL || write_cb_para->value == NULL) {
        return;
    }
    (void)team_handle_received_packet(conn_id, 0U, write_cb_para->value, write_cb_para->length);
}
static void team_client_rx_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    unused(client_id);
    if (status != ERRCODE_SLE_SUCCESS || data == NULL || data->data == NULL) {
        return;
    }
    (void)team_handle_received_packet(conn_id, 1U, data->data, data->data_len);
}
void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    team_client_rx_cb(client_id, conn_id, data, status);
}
void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status)
{
    team_client_rx_cb(client_id, conn_id, data, status);
}
static uint8_t team_client_seek_filter(const sle_seek_result_info_t *seek_result_data, void *user_ctx)
{
    static const char server_name[] = "sle_uart_server";
    uint8_t route_id;
    uint16_t fw_compat;

    unused(user_ctx);
    if (seek_result_data == NULL || seek_result_data->data == NULL || seek_result_data->data_length == 0U) {
        return 0U;
    }
    if (team_buffer_contains(seek_result_data->data, seek_result_data->data_length,
        server_name, sizeof(server_name) - 1U) == 0U) {
        return 0U;
    }
    route_id = team_route_id_from_adv_data(seek_result_data->data, seek_result_data->data_length);
    if (route_id == 0U) {
        route_id = team_route_id_from_sle_addr(seek_result_data->addr.addr);
    }
    if (route_id == 0U || route_id == SLE_TEAM_BROADCAST_ID || route_id == g_team_rt.route_id) {
        return 0U;
    }
    fw_compat = team_fw_compat_from_adv_data(seek_result_data->data, seek_result_data->data_length);
    if (fw_compat != SLE_TEAM_FW_COMPAT) {
        osal_printk("[team] seek skip route=%u fw_compat=0x%x expected=0x%x\r\n",
            route_id, fw_compat, (uint16_t)SLE_TEAM_FW_COMPAT);
        return 0U;
    }
    if (g_team_rt.role_configured == 0U) {
        return 0U;
    }
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER) {
        if (g_team_rt.relay_client_started == 0U || g_team_node.cfg.relay_enabled == 0U ||
            route_id == g_team_node.cfg.leader_id) {
            return 0U;
        }
    } else if (team_leader_should_connect_candidate(route_id) == 0U) {
        return 0U;
    }
    team_pending_note(&seek_result_data->addr, route_id);
    osal_printk("[team] seek accept route=%u role=%u rssi=%d\r\n",
        route_id, g_team_node.cfg.role, seek_result_data->rssi);
    return 1U;
}
static uint8_t team_event_is_client_side(uint16_t conn_id, const sle_addr_t *addr)
{
    uint8_t route_id = 0U;

    if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        return 1U;
    }
    if (sle_uart_client_has_conn(conn_id) != 0U) {
        return 1U;
    }
    if (addr != NULL && (team_pending_take(addr, &route_id) != 0U ||
        sle_uart_client_is_pending_remote_addr(addr) != 0U)) {
        if (route_id != 0U) {
            team_pending_note(addr, route_id);
        }
        return 1U;
    }
    return 0U;
}
static void team_member_drop_downstream_children(const char *reason)
{
    uint16_t conn_ids[SLE_TEAM_MAX_DIRECT_CONNECTIONS];
    uint8_t count;
    uint8_t i;

    count = sle_uart_client_get_active_conns(conn_ids, (uint8_t)SLE_TEAM_MAX_DIRECT_CONNECTIONS);
    for (i = 0U; i < count; i++) {
        uint8_t member_id = 0U;

        (void)sle_uart_client_get_conn_member(conn_ids[i], &member_id);
        (void)sle_uart_client_disconnect_conn(conn_ids[i]);
        osal_printk("[team] drop child conn=%u member=%u reason=%s\r\n",
            conn_ids[i], member_id, reason != NULL ? reason : "unknown");
    }
}
static void team_member_disable_relay_client(const char *reason)
{
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return;
    }
    if (g_team_rt.relay_client_started == 0U && g_team_node.cfg.relay_enabled == 0U) {
        return;
    }
    team_member_drop_downstream_children(reason != NULL ? reason : "relay-disabled");
    sle_uart_client_pause_scan(reason != NULL ? reason : "relay-disabled");
    g_team_node.cfg.relay_enabled = 0U;
    g_team_rt.relay_scan_paused_for_upstream_loss = 0U;
    team_print("relay child client disabled");
    team_status_led_update();
}
static void team_connection_state_changed_cbk(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    uint8_t use_client = team_event_is_client_side(conn_id, addr);
    uint8_t route_id = 0U;
    uint8_t disconnected_member_id = 0U;
    if (use_client != 0U) {
        if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
            disconnected_member_id = team_leader_resolve_disconnected_member(conn_id, addr);
        }
        sle_uart_client_handle_connect_state_changed(conn_id, addr, conn_state, pair_state, disc_reason);
        if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
            if (disconnected_member_id == 0U) { disconnected_member_id = team_leader_resolve_disconnected_member(conn_id, addr); }
            if (sle_uart_client_has_conn(conn_id) != 0U) {
                osal_printk("[team] disconnect ignored conn=%u route=%u\r\n", conn_id,
                    addr != NULL ? team_route_id_from_mac(addr->addr) : 0U);
                return;
            }
            if (disconnected_member_id != 0U) {
                if (team_leader_known_relay_child(disconnected_member_id) == 0U) {
                    osal_printk("[team] disconnect resolved conn=%u member=%u\r\n", conn_id, disconnected_member_id);
                    (void)sle_team_node_member_offline(&g_team_node, disconnected_member_id);
                } else {
                    osal_printk("[team] disconnect keep relay child conn=%u member=%u\r\n",
                        conn_id, disconnected_member_id);
                }
                g_team_rt.leader_scan_paused = 0U;
                sle_uart_client_resume_scan("leader-direct-lost");
                sle_uart_client_force_rescan();
            } else if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
                osal_printk("[team] disconnect unresolved conn=%u route=%u\r\n",
                    conn_id, addr != NULL ? team_route_id_from_mac(addr->addr) : 0U);
            }
        }
        if (conn_state == SLE_ACB_STATE_CONNECTED && addr != NULL && team_pending_take(addr, &route_id) != 0U &&
            route_id != 0U) {
            (void)sle_uart_client_bind_member_conn(route_id, conn_id);
            osal_printk("[team] client conn bind conn=%u route=%u\r\n", conn_id, route_id);
        }
        return;
    }
    if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        (void)sle_uart_server_get_conn_member(conn_id, &disconnected_member_id);
    }
    sle_uart_server_handle_connect_state_changed(conn_id, addr, conn_state, pair_state, disc_reason);
    if (conn_state == SLE_ACB_STATE_DISCONNECTED && g_team_rt.role_configured != 0U &&
        g_team_node.cfg.role == SLE_TEAM_ROLE_MEMBER && g_team_node.joined != 0U) {
        uint8_t upstream_lost = (uint8_t)(disconnected_member_id != 0U &&
            disconnected_member_id == g_team_node.upstream_parent_id);
        if (upstream_lost == 0U) {
            if (disconnected_member_id != 0U) {
                osal_printk("[team] stale upstream disconnect conn=%u old_parent=%u current_parent=%u\r\n",
                    conn_id, disconnected_member_id, g_team_node.upstream_parent_id);
            } else if (sle_uart_server_connected_count() == 0U) {
                osal_printk("[team] disconnect ignored conn=%u no bound upstream while joined parent=%u\r\n",
                    conn_id, g_team_node.upstream_parent_id);
            }
            return;
        }
        ws63_team_status_led_lost();
        team_display_publish_event(WS63_ST7789_EVENT_LOST, g_team_node.upstream_parent_id, NULL);
        (void)sle_team_node_member_link_lost(&g_team_node);
        team_member_drop_downstream_children("upstream-lost");
        if (g_team_rt.relay_client_started != 0U) {
            g_team_rt.relay_scan_paused_for_upstream_loss = 1U;
            sle_uart_client_pause_scan_request("upstream-lost");
        }
    }
}
static void team_pair_complete_cbk(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    if (team_event_is_client_side(conn_id, addr) != 0U) {
        sle_uart_client_handle_pair_complete(conn_id, addr, status);
    } else {
        sle_uart_server_handle_pair_complete(conn_id, addr, status);
    }
}
static void team_read_rssi_cbk(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    if (team_event_is_client_side(conn_id, NULL) != 0U) {
        sle_uart_client_handle_read_rssi(conn_id, rssi, status);
    } else {
        sle_uart_server_handle_read_rssi(conn_id, rssi, status);
    }
}
static void team_register_connection_callbacks(void)
{
    errcode_t ret;

    (void)memset_s(&g_team_conn_cbks, sizeof(g_team_conn_cbks), 0, sizeof(g_team_conn_cbks));
    g_team_conn_cbks.connect_state_changed_cb = team_connection_state_changed_cbk;
    g_team_conn_cbks.pair_complete_cb = team_pair_complete_cbk;
    g_team_conn_cbks.read_rssi_cb = team_read_rssi_cbk;
    ret = sle_connection_register_callbacks(&g_team_conn_cbks);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[team] connection callback register failed ret=0x%x\r\n", ret);
    }
}
static void team_node_init(sle_team_node_role_t role, uint8_t team_id, uint8_t leader_id, uint8_t channel_hash,
    uint16_t leader_term)
{
    sle_team_node_cfg_t cfg;
    sle_team_node_ops_t ops;

    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    (void)memset_s(&ops, sizeof(ops), 0, sizeof(ops));
    cfg.team_id = team_id;
    cfg.self_id = g_team_rt.route_id != 0U ? g_team_rt.route_id : (uint8_t)CONFIG_SLE_TEAM_SELF_ID;
    cfg.leader_id = leader_id != 0U ? leader_id : (uint8_t)CONFIG_SLE_TEAM_LEADER_ID;
    cfg.leader_term = leader_term != 0U ? leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    cfg.role = role;
    cfg.channel_hash = channel_hash;
    cfg.default_ttl = 4U;
    cfg.fw_compat = (uint16_t)SLE_TEAM_FW_COMPAT;
    cfg.report_interval_s = (uint16_t)CONFIG_SLE_TEAM_REPORT_INTERVAL_S;
    cfg.heartbeat_interval_s = (uint16_t)CONFIG_SLE_TEAM_HEARTBEAT_INTERVAL_S;
    cfg.warn_distance_m = (uint16_t)CONFIG_SLE_TEAM_WARN_DISTANCE_M;
    cfg.lost_distance_m = (uint16_t)CONFIG_SLE_TEAM_LOST_DISTANCE_M;
    cfg.heartbeat_timeout_s = (uint16_t)CONFIG_SLE_TEAM_HEARTBEAT_TIMEOUT_S;
    cfg.parent_timeout_s = (uint16_t)CONFIG_SLE_TEAM_HEARTBEAT_TIMEOUT_S;
    cfg.max_downstream = role == SLE_TEAM_ROLE_LEADER ? team_direct_cap() : 0U;
    if (g_team_rt.self_mac_ready != 0U) {
        (void)memcpy_s(cfg.self_mac, sizeof(cfg.self_mac), g_team_rt.self_mac, sizeof(g_team_rt.self_mac));
        cfg.self_mac_ready = 1U;
    }
    ops.send = team_sle_send;
    ops.now_s = team_now_s;
    ops.rssi_dbm = team_rssi_dbm;
    ops.battery_percent = team_battery_percent;
    ops.log = team_node_log;
    ops.on_joined = team_on_joined;
    ops.on_position = team_on_position;
    ops.on_alert = team_on_alert;
    ops.on_relay_offline = team_on_relay_offline;
    ops.should_defer_member_timeout = team_member_timeout_defer;
    (void)sle_team_node_init(&g_team_node, &cfg, &ops); g_team_rt.last_relay_optimize_ms = 0U;
    sle_team_cli_init(&g_team_cli, &g_team_node, team_cli_print, NULL);
    sle_uart_server_adv_set_route_id(g_team_node.cfg.self_id);
    sle_uart_server_adv_set_fw_compat(g_team_node.cfg.fw_compat);
}
static int team_sle_start(void)
{
    errcode_t ret;

    if (g_team_rt.sle_started != 0U) { return SLE_TEAM_OK; }
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        sle_uart_client_set_connect_limit(team_physical_connect_limit());
        sle_uart_client_set_seek_filter(team_client_seek_filter, NULL);
        sle_uart_client_init(team_client_rx_cb, team_client_rx_cb);
        team_register_connection_callbacks();
        g_team_rt.sle_started = 1U;
        team_print("leader client started");
        return SLE_TEAM_OK;
    }
    team_sle_prepare_local_addr();
    ret = sle_uart_server_init(team_server_read_cb, team_server_write_cb);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("[team] member server init failed ret=0x%x\r\n", ret);
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    team_register_connection_callbacks();
    g_team_rt.sle_started = 1U;
    team_print("member server started");
    return SLE_TEAM_OK;
}
static int team_configure_role(sle_team_node_role_t role, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_term)
{
    int ret;

    if (g_team_rt.role_configured != 0U) { return SLE_TEAM_ERR_UNSUPPORTED; }
    team_node_init(role, team_id, leader_id, channel_hash, leader_term);
    ret = team_sle_start();
    if (ret != SLE_TEAM_OK) {
        (void)memset_s(&g_team_node, sizeof(g_team_node), 0, sizeof(g_team_node));
        return ret;
    }
    g_team_rt.role_configured = 1U;
    g_team_rt.last_role_ret = SLE_TEAM_OK;
    team_identity_refresh_labels();
    osal_printk("[team] configured fw=%s role=%s self=%u leader=%u team=%u channel=%u direct_cap=%u term=%u label=%s\r\n",
        SLE_TEAM_FW_VERSION, role == SLE_TEAM_ROLE_LEADER ? "leader" : "member",
        g_team_node.cfg.self_id, g_team_node.cfg.leader_id, g_team_node.cfg.team_id,
        g_team_node.cfg.channel_hash, team_direct_cap(), g_team_node.cfg.leader_term, g_team_rt.self_label);
    if (role == SLE_TEAM_ROLE_MEMBER) {
        ret = sle_team_node_member_select_leader_term(&g_team_node, team_id, leader_id,
            channel_hash, g_team_node.cfg.leader_term);
        osal_printk("[team] member hello ret=%d\r\n", ret);
    }
    team_status_led_update();
    team_display_publish_status();
    return SLE_TEAM_OK;
}
static void team_relay_client_start_if_ready(void)
{
    if (g_team_rt.role_configured == 0U || g_team_node.cfg.role != SLE_TEAM_ROLE_MEMBER) {
        return;
    }
    if (g_team_node.joined == 0U || g_team_node.cfg.relay_allowed == 0U) {
        team_member_disable_relay_client("relay-not-allowed");
        return;
    }
    if (g_team_rt.relay_client_started != 0U) {
        if (g_team_node.cfg.relay_enabled != 0U && g_team_rt.relay_scan_paused_for_upstream_loss != 0U) {
            sle_uart_client_set_connect_limit(SLE_TEAM_RELAY_CHILD_CAP_DEFAULT);
            sle_uart_client_set_seek_filter(team_client_seek_filter, NULL);
            sle_uart_client_resume_scan("relay-recovery-ready");
            sle_uart_client_force_rescan();
            g_team_rt.relay_scan_paused_for_upstream_loss = 0U;
            team_print("relay child client resumed");
        }
        return;
    }
    sle_uart_client_set_connect_limit(SLE_TEAM_RELAY_CHILD_CAP_DEFAULT);
    sle_uart_client_set_seek_filter(team_client_seek_filter, NULL);
    sle_uart_client_init(team_client_rx_cb, team_client_rx_cb);
    team_register_connection_callbacks();
    g_team_rt.relay_client_started = 1U;
    g_team_rt.relay_scan_paused_for_upstream_loss = 0U;
    g_team_node.cfg.relay_enabled = 1U;
    team_print("relay child client started");
    team_status_led_update();
    team_display_publish_status();
}
static void team_cfg_status_json(void)
{
    sle_team_config_nv_t cfg;
    uint8_t nv_valid;
    uint8_t runtime_valid = g_team_rt.role_configured != 0U ? 1U : 0U;
    uint8_t runtime_role = runtime_valid != 0U ? (uint8_t)g_team_node.cfg.role : 0xFFU;
    uint8_t runtime_self = runtime_valid != 0U ? g_team_node.cfg.self_id : g_team_rt.route_id;
    char json[1024];

    nv_valid = team_nv_config_load(&cfg) == SLE_TEAM_OK ? 1U : 0U;
    if (nv_valid == 0U) {
        (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
        cfg.role = 0xFFU;
    }
    (void)snprintf(json, sizeof(json),
        "{\"ok\":true,\"fw\":\"%s\",\"profile\":\"minimal\",\"selfSuffix\":\"%04X\",\"routeId\":%u,"
        "\"nvValid\":%s,\"nvRole\":\"%s\",\"nvRoleValue\":%u,\"nvTeam\":%u,\"nvChannel\":%u,"
        "\"nvLeaderSuffix\":\"%04X\",\"nvLeaderTerm\":%u,\"runtimeConfigured\":%s,\"runtimeRole\":\"%s\","
        "\"runtimeRoleValue\":%u,\"runtimeTeam\":%u,\"runtimeChannel\":%u,\"runtimeLeader\":%u,"
        "\"runtimeLeaderTerm\":%u,\"runtimeSelf\":%u,\"runtimeDirectCap\":%u,\"runtimeRelayTarget\":%u,"
        "\"runtimeRelayCount\":%u,\"runtimeOnlineCount\":%u,\"runtimeJoined\":%u,"
        "\"runtimeParent\":%u,\"runtimeRelayEnabled\":%u,"
        "\"roleRequestPending\":%s,\"roleRequestRole\":\"%s\",\"roleRequestRoleValue\":%u,"
        "\"roleRequestTeam\":%u,\"roleRequestChannel\":%u,\"roleRequestLeader\":%u,"
        "\"roleRequestLeaderSuffix\":\"%04X\",\"roleRequestLeaderTerm\":%u,"
        "\"roleRequestLastRet\":%d,\"lastRoleRet\":%d}",
        SLE_TEAM_FW_VERSION,
        team_self_mac_suffix(),
        g_team_rt.route_id,
        nv_valid != 0U ? "true" : "false",
        team_role_name(cfg.role, nv_valid),
        nv_valid != 0U ? cfg.role : 255U,
        nv_valid != 0U ? cfg.team_id : 0U,
        nv_valid != 0U ? cfg.channel_hash : 0U,
        nv_valid != 0U ? cfg.leader_suffix : 0U,
        nv_valid != 0U ? cfg.leader_term : 0U,
        runtime_valid != 0U ? "true" : "false",
        team_role_name(runtime_role, runtime_valid),
        runtime_role,
        runtime_valid != 0U ? g_team_node.cfg.team_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.channel_hash : 0U,
        runtime_valid != 0U ? g_team_node.cfg.leader_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.leader_term : 0U,
        runtime_self,
        runtime_valid != 0U && runtime_role == (uint8_t)SLE_TEAM_ROLE_LEADER ? team_direct_cap() : 0U,
        runtime_valid != 0U && runtime_role == (uint8_t)SLE_TEAM_ROLE_LEADER ? g_team_rt.relay_target : 0U,
        team_relay_count(),
        team_online_count(),
        runtime_valid != 0U ? g_team_node.joined : 0U,
        runtime_valid != 0U ? g_team_node.upstream_parent_id : 0U,
        runtime_valid != 0U ? g_team_node.cfg.relay_enabled : 0U,
        g_team_rt.role_request_pending != 0U ? "true" : "false",
        team_role_name(g_team_rt.role_request_role, g_team_rt.role_request_pending),
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_role : 255U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_team : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_channel : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader_suffix : 0U,
        g_team_rt.role_request_pending != 0U ? g_team_rt.role_request_leader_term : 0U,
        g_team_rt.role_request_last_ret,
        g_team_rt.last_role_ret);
    osal_printk("[cfg-json] %s\r\n", json);
}
static int team_cfg_done(void) { team_cfg_status_json(); return 1; }
static int team_cfg_apply_loaded(const sle_team_config_nv_t *cfg)
{
    /* User/API initiated apply uses the queued path so Web can show starting
     * before the role is fully brought up by the main loop. */
    if (cfg == NULL || team_nv_config_valid(cfg) == 0U) { return SLE_TEAM_ERR_ARG; }
    if (cfg->role == (uint8_t)SLE_TEAM_ROLE_LEADER) {
        return team_request_role_config(SLE_TEAM_ROLE_LEADER, cfg->team_id, g_team_rt.route_id,
            cfg->channel_hash, team_self_mac_suffix(), cfg->leader_term, cfg->direct_cap, 1U);
    }
    return team_request_role_config(SLE_TEAM_ROLE_MEMBER, cfg->team_id,
        team_route_id_from_suffix(cfg->leader_suffix), cfg->channel_hash, cfg->leader_suffix,
        cfg->leader_term, 0U, 1U);
}
static int team_cfg_restore_loaded(const sle_team_config_nv_t *cfg)
{
    uint8_t leader_id;

    /* Boot restore keeps the old direct path so saved config comes up immediately
     * during startup before the CLI/Web surfaces are fully live. */
    if (cfg == NULL || team_nv_config_valid(cfg) == 0U) { return SLE_TEAM_ERR_ARG; }
    if (cfg->role == (uint8_t)SLE_TEAM_ROLE_LEADER) {
        g_team_rt.direct_cap = cfg->direct_cap;
        team_identity_set_leader_suffix(team_self_mac_suffix());
        return team_configure_role(SLE_TEAM_ROLE_LEADER, cfg->team_id, g_team_rt.route_id,
            cfg->channel_hash, cfg->leader_term);
    }
    leader_id = team_route_id_from_suffix(cfg->leader_suffix);
    team_identity_set_leader_suffix(cfg->leader_suffix);
    return team_configure_role(SLE_TEAM_ROLE_MEMBER, cfg->team_id, leader_id,
        cfg->channel_hash, cfg->leader_term);
}
static int team_cfg_save_leader(uint8_t team_id, uint8_t channel_hash, uint16_t leader_term, uint8_t apply_now)
{
    int ret = team_nv_config_save(SLE_TEAM_ROLE_LEADER, team_id, team_self_mac_suffix(),
        channel_hash, team_direct_cap(), leader_term);

    if (ret == SLE_TEAM_OK && apply_now != 0U) {
        ret = team_request_role_config(SLE_TEAM_ROLE_LEADER, team_id, g_team_rt.route_id,
            channel_hash, team_self_mac_suffix(), leader_term, team_direct_cap(), 1U);
    }
    return ret;
}
static int team_cfg_save_member(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash,
    uint16_t leader_term, uint8_t apply_now)
{
    int ret = team_nv_config_save(SLE_TEAM_ROLE_MEMBER, team_id, leader_suffix, channel_hash, 0U, leader_term);

    if (ret == SLE_TEAM_OK && apply_now != 0U) {
        ret = team_request_role_config(SLE_TEAM_ROLE_MEMBER, team_id,
            team_route_id_from_suffix(leader_suffix), channel_hash, leader_suffix, leader_term, 0U, 1U);
    }
    return ret;
}
static int team_request_role_config(sle_team_node_role_t role, uint8_t team_id, uint8_t leader_id,
    uint8_t channel_hash, uint16_t leader_suffix, uint16_t leader_term, uint8_t direct_cap, uint8_t save_nv)
{
    if (g_team_rt.role_configured != 0U || g_team_rt.role_request_pending != 0U) {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    g_team_rt.role_request_role = (uint8_t)role;
    g_team_rt.role_request_leader = leader_id;
    g_team_rt.role_request_team = team_id;
    g_team_rt.role_request_channel = channel_hash;
    g_team_rt.role_request_leader_suffix = leader_suffix;
    g_team_rt.role_request_leader_term = leader_term != 0U ? leader_term : SLE_TEAM_LEADER_TERM_DEFAULT;
    g_team_rt.role_request_direct_cap = direct_cap;
    g_team_rt.role_request_save_nv = save_nv;
    g_team_rt.role_request_last_ret = SLE_TEAM_OK;
    g_team_rt.role_request_pending = 1U;
    if (role == SLE_TEAM_ROLE_LEADER) {
        team_identity_set_leader_suffix(team_self_mac_suffix());
    } else {
        team_identity_set_leader_suffix(leader_suffix);
    }
    team_identity_refresh_labels();
    team_status_led_update();
    team_display_publish_status();
    osal_printk("[team] role request queued role=%u leader=%u team=%u channel=%u suffix=%04X term=%u direct_cap=%u save_nv=%u\r\n",
        (uint8_t)role, leader_id, team_id, channel_hash, leader_suffix, g_team_rt.role_request_leader_term,
        direct_cap, save_nv);
    return SLE_TEAM_OK;
}
static void team_handle_role_request_once(void)
{
    sle_team_node_role_t role;
    uint8_t team_id;
    uint8_t leader_id;
    uint8_t channel_hash;
    uint16_t leader_suffix;
    uint16_t leader_term;
    uint8_t direct_cap;
    uint8_t save_nv;
    int ret;

    if (g_team_rt.role_request_pending == 0U || g_team_rt.role_configured != 0U) {
        return;
    }
    role = (sle_team_node_role_t)g_team_rt.role_request_role;
    team_id = g_team_rt.role_request_team;
    leader_id = g_team_rt.role_request_leader;
    channel_hash = g_team_rt.role_request_channel;
    leader_suffix = g_team_rt.role_request_leader_suffix;
    leader_term = g_team_rt.role_request_leader_term;
    direct_cap = g_team_rt.role_request_direct_cap;
    save_nv = g_team_rt.role_request_save_nv;
    if (role == SLE_TEAM_ROLE_LEADER) {
        g_team_rt.direct_cap = direct_cap != 0U ? direct_cap : g_team_rt.direct_cap;
        ret = team_configure_role(SLE_TEAM_ROLE_LEADER, team_id, leader_id, channel_hash, leader_term);
    } else {
        ret = team_configure_role(SLE_TEAM_ROLE_MEMBER, team_id, leader_id, channel_hash, leader_term);
    }
    g_team_rt.role_request_last_ret = ret;
    g_team_rt.last_role_ret = ret;
    g_team_rt.role_request_pending = 0U;
    if (ret == SLE_TEAM_OK && save_nv != 0U) {
        if (role == SLE_TEAM_ROLE_LEADER) {
            (void)team_nv_config_save(SLE_TEAM_ROLE_LEADER, team_id, team_self_mac_suffix(),
                channel_hash, g_team_rt.direct_cap, leader_term);
        } else {
            (void)team_nv_config_save(SLE_TEAM_ROLE_MEMBER, team_id, leader_suffix, channel_hash, 0U, leader_term);
        }
    }
    osal_printk("[team] role request done role=%u leader=%u team=%u channel=%u suffix=%04X term=%u ret=%d\r\n",
        (uint8_t)role, leader_id, team_id, channel_hash, leader_suffix, leader_term, ret);
}
static void *team_reboot_task(const char *arg)
{
    const char *reason = arg != NULL ? arg : "manual";
    osal_msleep(800);
    osal_printk("[team] reboot now reason=%s\r\n", reason);
    (void)uapi_watchdog_init(1);
    (void)uapi_watchdog_set_time(1);
    (void)uapi_watchdog_enable(WDT_MODE_RESET);
    while (1) {}
    return NULL;
}
static void team_reboot_schedule(const char *reason)
{
    osal_task *task = NULL;

    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)team_reboot_task, (void *)reason, "TeamReboot",
        SLE_TEAM_REBOOT_TASK_STACK_SIZE);
    if (task != NULL) {
        osal_kthread_set_priority(task, SLE_TEAM_REBOOT_TASK_PRIO);
    }
    osal_kthread_unlock();
}
static int team_cfg_cli_handle(const char *line)
{
    unsigned int team_id = 0U;
    unsigned int channel = 0U;
    unsigned int suffix = 0U;
    unsigned int direct_cap = 0U;
    unsigned int relay_target = 0U;
    unsigned int leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    int ret;

    if (line == NULL || strncmp(line, "cfg", 3) != 0) {
        return 0;
    }
    if (strcmp(line, "cfg") == 0 || strcmp(line, "cfg status") == 0) {
        return team_cfg_done();
    }
    if (strcmp(line, "cfg clear") == 0) {
        int ret_cfg = team_nv_config_clear();
        int ret_allow = team_nv_allowed_clear();

        osal_printk("[cfg] clear ret=%d config=%d allow=%d\r\n",
            (ret_cfg == SLE_TEAM_OK && ret_allow == SLE_TEAM_OK) ? SLE_TEAM_OK : SLE_TEAM_ERR_UNSUPPORTED,
            ret_cfg, ret_allow);
        return team_cfg_done();
    }
    if (strcmp(line, "cfg reboot") == 0) {
        osal_printk("[cfg] reboot scheduled\r\n");
        team_reboot_schedule("cfg-cli");
        return 1;
    }
    if ((sscanf(line, "cfg leader now %u %u %u", &team_id, &channel, &leader_term) == 3 ||
        sscanf(line, "cfg leader now %u %u", &team_id, &channel) == 2) &&
        team_id >= 1U && team_id <= 254U && channel <= 255U && leader_term <= 65535U) {
        ret = team_cfg_save_leader((uint8_t)team_id, (uint8_t)channel, (uint16_t)leader_term, 1U);
        osal_printk("[cfg] leader-now ret=%d team=%u channel=%u term=%u self_suffix=%04X\r\n",
            ret, team_id, channel, leader_term, team_self_mac_suffix());
        return team_cfg_done();
    }
    leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    if ((sscanf(line, "cfg leader %u %u %u", &team_id, &channel, &leader_term) == 3 ||
        sscanf(line, "cfg leader %u %u", &team_id, &channel) == 2) &&
        team_id >= 1U && team_id <= 254U && channel <= 255U && leader_term <= 65535U) {
        ret = team_cfg_save_leader((uint8_t)team_id, (uint8_t)channel, (uint16_t)leader_term, 0U);
        osal_printk("[cfg] save leader ret=%d team=%u channel=%u term=%u self_suffix=%04X\r\n",
            ret, team_id, channel, leader_term, team_self_mac_suffix());
        return team_cfg_done();
    }
    leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    if ((sscanf(line, "cfg member now %x %u %u %u", &suffix, &team_id, &channel, &leader_term) == 4 ||
        sscanf(line, "cfg member now %x %u %u", &suffix, &team_id, &channel) == 3) &&
        suffix >= 1U && suffix <= 0xFFFFU && team_id >= 1U && team_id <= 254U &&
        channel <= 255U && leader_term <= 65535U) {
        ret = team_cfg_save_member((uint16_t)suffix, (uint8_t)team_id,
            (uint8_t)channel, (uint16_t)leader_term, 1U);
        osal_printk("[cfg] member-now ret=%d leader_suffix=%04X leader=%u team=%u channel=%u term=%u\r\n",
            ret, suffix, team_route_id_from_suffix((uint16_t)suffix), team_id, channel, leader_term);
        return team_cfg_done();
    }
    leader_term = SLE_TEAM_LEADER_TERM_DEFAULT;
    if ((sscanf(line, "cfg member %x %u %u %u", &suffix, &team_id, &channel, &leader_term) == 4 ||
        sscanf(line, "cfg member %x %u %u", &suffix, &team_id, &channel) == 3) &&
        suffix >= 1U && suffix <= 0xFFFFU && team_id >= 1U && team_id <= 254U &&
        channel <= 255U && leader_term <= 65535U) {
        ret = team_cfg_save_member((uint16_t)suffix, (uint8_t)team_id,
            (uint8_t)channel, (uint16_t)leader_term, 0U);
        osal_printk("[cfg] save member ret=%d leader_suffix=%04X leader=%u team=%u channel=%u term=%u\r\n",
            ret, suffix, team_route_id_from_suffix((uint16_t)suffix), team_id, channel, leader_term);
        return team_cfg_done();
    }
    if (strcmp(line, "cfg direct") == 0) {
        osal_printk("[cfg] direct cap=%u hw_max=%u ret=0\r\n",
            team_direct_cap(), SLE_TEAM_MAX_DIRECT_CONNECTIONS);
        return team_cfg_done();
    }
    if (sscanf(line, "cfg direct %u", &direct_cap) == 1) {
        if (direct_cap < 1U || direct_cap >= SLE_TEAM_MAX_DIRECT_CONNECTIONS) {
            osal_printk("[cfg] direct ret=%d reason=range value=%u max_user=%u\r\n",
                SLE_TEAM_ERR_ARG, direct_cap, SLE_TEAM_MAX_DIRECT_CONNECTIONS - 1U);
            return team_cfg_done();
        }
        g_team_rt.direct_cap = (uint8_t)direct_cap;
        if (g_team_rt.role_configured != 0U && g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
            g_team_node.cfg.max_downstream = (uint8_t)direct_cap;
            sle_uart_client_set_connect_limit(team_physical_connect_limit());
            (void)team_nv_config_save(SLE_TEAM_ROLE_LEADER, g_team_node.cfg.team_id, team_self_mac_suffix(),
                g_team_node.cfg.channel_hash, g_team_rt.direct_cap, g_team_node.cfg.leader_term);
        }
        osal_printk("[cfg] direct cap=%u hw_max=%u ret=0\r\n",
            team_direct_cap(), SLE_TEAM_MAX_DIRECT_CONNECTIONS);
        return team_cfg_done();
    }
    if (strcmp(line, "cfg relay") == 0) {
        osal_printk("[cfg] relay target=%u relays=%u ret=0\r\n", g_team_rt.relay_target, team_relay_count());
        return team_cfg_done();
    }
    if (sscanf(line, "cfg relay target %u", &relay_target) == 1 && relay_target <= SLE_TEAM_MAX_MEMBERS) {
        g_team_rt.relay_target = (uint8_t)relay_target;
        team_leader_fill_relay_target();
        osal_printk("[cfg] relay target=%u override=%u ret=0\r\n",
            g_team_rt.relay_target, g_team_rt.relay_target);
        return team_cfg_done();
    }
    if (strcmp(line, "cfg apply") == 0) {
        sle_team_config_nv_t cfg;

        if (team_nv_config_load(&cfg) != SLE_TEAM_OK) {
            osal_printk("[cfg] apply ret=%d reason=nv-empty\r\n", SLE_TEAM_ERR_FORMAT);
            return team_cfg_done();
        }
        ret = team_cfg_apply_loaded(&cfg);
        osal_printk("[cfg] apply ret=%d role=%u team=%u channel=%u leader_suffix=%04X\r\n",
            ret, cfg.role, cfg.team_id, cfg.channel_hash, cfg.leader_suffix);
        return team_cfg_done();
    }
    if (strcmp(line, "cfg help") == 0) {
        osal_printk("[cfg] cmds: status | leader now <team> <channel> | member now <leader_suffix_hex> <team> <channel>\r\n");
        osal_printk("[cfg]       direct <1-7> | relay target <n> | apply | clear | reboot\r\n");
        return 1;
    }
    return 0;
}
static void team_uart_pins_init(void)
{
    (void)uapi_pin_set_mode(CONFIG_SLE_TEAM_UART_TXD_PIN, PIN_MODE_1);
    (void)uapi_pin_set_mode(CONFIG_SLE_TEAM_UART_RXD_PIN, PIN_MODE_1);
}
static void team_uart_init(void)
{
    uart_attr_t attr = {
        .baud_rate = SLE_TEAM_UART_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE,
    };
    uart_pin_config_t pin_config = {
        .tx_pin = CONFIG_SLE_TEAM_UART_TXD_PIN,
        .rx_pin = CONFIG_SLE_TEAM_UART_RXD_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };

    team_uart_pins_init();
    (void)uapi_uart_deinit(CONFIG_SLE_TEAM_UART_BUS);
    (void)uapi_uart_init(CONFIG_SLE_TEAM_UART_BUS, &pin_config, &attr, NULL, &g_uart_buffer_config);
}
static void team_cli_enqueue_line(const char *line)
{
    sle_team_cli_msg_t msg;
    int ret;

    if (line == NULL || g_team_rt.cli_queue_ready == 0U) {
        return;
    }
    (void)memset_s(&msg, sizeof(msg), 0, sizeof(msg));
    (void)snprintf(msg.line, sizeof(msg.line), "%s", line);
    ret = osal_msg_queue_write_copy(g_team_rt.cli_queue_id, &msg, (uint32_t)sizeof(msg), 0);
    if (ret != OSAL_SUCCESS) {
        osal_printk("[cli] queue full drop=%s\r\n", line);
    }
}
static void team_uart_rx_cb(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *data = (const uint8_t *)buffer;
    uint16_t i;

    if (error || data == NULL) {
        return;
    }
    for (i = 0U; i < length; i++) {
        char ch = (char)data[i];

        if (ch == '\r' || ch == '\n') {
            if (g_team_rt.line_len > 0U) {
                g_team_rt.line_buf[g_team_rt.line_len] = '\0';
                team_cli_enqueue_line(g_team_rt.line_buf);
                g_team_rt.line_len = 0U;
            }
            continue;
        }
        if (g_team_rt.line_len + 1U < sizeof(g_team_rt.line_buf)) {
            g_team_rt.line_buf[g_team_rt.line_len++] = ch;
        } else {
            g_team_rt.line_len = 0U;
            team_print("cli line too long");
        }
    }
}
static void team_uart_cli_start(void)
{
    errcode_t ret;

    if (osal_msg_queue_create("team_cli_q", SLE_TEAM_CLI_QUEUE_LEN, &g_team_rt.cli_queue_id, 0,
        sizeof(sle_team_cli_msg_t)) == OSAL_SUCCESS) {
        g_team_rt.cli_queue_ready = 1U;
    } else {
        team_print("cli queue create failed");
    }
    (void)uapi_uart_unregister_rx_callback(CONFIG_SLE_TEAM_UART_BUS);
    ret = uapi_uart_register_rx_callback(CONFIG_SLE_TEAM_UART_BUS, UART_RX_CONDITION_FULL_OR_IDLE, 1, team_uart_rx_cb);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[team] uart rx callback failed ret=0x%x\r\n", ret);
    }
}
static void team_handle_cli_queue_once(void)
{
    sle_team_cli_msg_t msg;
    uint32_t msg_size = sizeof(msg);

    if (g_team_rt.cli_queue_ready == 0U) {
        osal_msleep(SLE_TEAM_CLI_QUEUE_TIMEOUT_MS);
        return;
    }
    (void)memset_s(&msg, sizeof(msg), 0, sizeof(msg));
    if (osal_msg_queue_read_copy(g_team_rt.cli_queue_id, &msg, &msg_size, SLE_TEAM_CLI_QUEUE_TIMEOUT_MS) !=
        OSAL_SUCCESS) {
        return;
    }
    if (strcmp(msg.line, "reboot") == 0 || strcmp(msg.line, "reset") == 0) {
        team_reboot_schedule("cli");
        return;
    }
    if (ws63_team_power_cli_handle(msg.line) != 0) {
        return;
    }
    if (team_cfg_cli_handle(msg.line) != 0) {
        return;
    }
    if (g_team_rt.role_configured == 0U) {
        osal_printk("[cli] unconfigured; use cfg leader now <team> <channel> or cfg member now <leader_suffix> <team> <channel>\r\n");
        return;
    }
    sle_team_cli_handle_line(&g_team_cli, msg.line);
}
static void team_restore_saved_config(void)
{
    sle_team_config_nv_t cfg;
    int ret;

    if (team_nv_config_load(&cfg) != SLE_TEAM_OK) {
        team_print("boot unconfigured");
        return;
    }
    ret = team_cfg_restore_loaded(&cfg);
    g_team_rt.last_role_ret = ret;
    osal_printk("[team-nv] restore role=%u team=%u channel=%u leader_suffix=%04X ret=%d\r\n",
        cfg.role, cfg.team_id, cfg.channel_hash, cfg.leader_suffix, ret);
}
static void team_network_task_bootstrap(void)
{
    /*
     * One-time board bring-up for the networking task:
     * runtime defaults, Web event log, LED/GPS, identity, SoftAP HTTP, UART CLI,
     * then any saved leader/member role from NV.
     */
    g_team_rt.direct_cap = SLE_TEAM_DIRECT_CAP_DEFAULT;
    g_team_rt.relay_target = 0U;
    g_team_rt.last_role_ret = SLE_TEAM_OK;
    sle_team_web_event_log_init(&g_team_events);
    ws63_team_status_led_init();
    ws63_team_gps_init();
    ws63_team_power_init();
    team_identity_init();
    ws63_team_http_start(&g_team_node, &g_team_events, &g_team_http_callbacks, g_team_rt.softap_ssid);
    team_uart_init();
    team_uart_cli_start();
    team_display_publish_status();
    osal_printk("[team] boot fw=%s profile=%s route=%u label=%s direct_cap=%u\r\n",
        SLE_TEAM_FW_VERSION, SLE_TEAM_HW_CONSTRAINTS, g_team_rt.route_id, g_team_rt.self_label, team_direct_cap());
    team_restore_saved_config();
    team_cfg_status_json();
}
static void team_network_tick_role_configured(void)
{
    uint32_t now_ms = (uint32_t)uapi_tcxo_get_ms();
    /*
     * Periodic runtime tick after a role exists. The portable core handles
     * membership timeouts and policy; the WS63 shell handles scan policy,
     * relay child-client start, GPS reports, LEDs, and display snapshots.
     */
    sle_team_node_tick(&g_team_node);
    ws63_team_power_tick(0U);
    if (g_team_node.cfg.role == SLE_TEAM_ROLE_LEADER) {
        team_leader_update_scan_policy();
        if (sle_team_relay_optimizer_tick(&g_team_node, now_ms, 8000U, &g_team_rt.last_relay_optimize_ms) != 0U) {
            osal_printk("[team] relay optimize rssi ret=1 relays=%u\r\n", team_relay_count());
            g_team_rt.leader_scan_paused = 0U;
        }
    }
    team_relay_client_start_if_ready();
    ws63_team_gps_tick(&g_team_node, now_ms, team_battery_percent(NULL));
    team_display_publish_status();
}
static void *team_network_task(const char *arg)
{
    unused(arg);
    team_network_task_bootstrap();
    while (1) {
        /* Keep CLI and SLE client progress flowing even before a role is set. */
        team_handle_cli_queue_once();
        sle_uart_client_tick();
        team_status_led_update();
        ws63_team_status_led_tick((uint32_t)uapi_tcxo_get_ms());
        team_handle_role_request_once();
        if (g_team_rt.role_configured != 0U) {
            team_network_tick_role_configured();
        }
        osal_msleep(SLE_TEAM_MAIN_LOOP_SLEEP_MS);
    }
    return NULL;
}
static int team_display_init_panel(void)
{
#if CONFIG_SLE_TEAM_ST7789_ENABLE
    ws63_st7789_config_t cfg;

    /* Current FPC wiring is supplied by Kconfig so old pin maps stay out of code. */
    (void)memset_s(&cfg, sizeof(cfg), 0, sizeof(cfg));
    cfg.spi_bus = (uint8_t)CONFIG_SLE_TEAM_ST7789_SPI_BUS;
    cfg.sclk_pin = (uint8_t)CONFIG_SLE_TEAM_ST7789_SCLK_PIN;
    cfg.mosi_pin = (uint8_t)CONFIG_SLE_TEAM_ST7789_MOSI_PIN;
    cfg.cs_pin = (uint8_t)CONFIG_SLE_TEAM_ST7789_CS_PIN;
    cfg.dc_pin = (uint8_t)CONFIG_SLE_TEAM_ST7789_DC_PIN;
    cfg.reset_pin = (uint8_t)CONFIG_SLE_TEAM_ST7789_RESET_PIN;
    cfg.x_offset = (uint16_t)CONFIG_SLE_TEAM_ST7789_X_OFFSET;
    cfg.y_offset = (uint16_t)CONFIG_SLE_TEAM_ST7789_Y_OFFSET;
    cfg.width = (uint16_t)CONFIG_SLE_TEAM_ST7789_WIDTH;
    cfg.height = (uint16_t)CONFIG_SLE_TEAM_ST7789_HEIGHT;
    return ws63_st7789_init(&cfg);
#else
    return -1;
#endif
}
static void *team_display_task(const char *arg)
{
    team_display_status_t status;
    team_display_event_t display_event;
    uint32_t last_status_seq = 0U;
    uint32_t last_event_seq = 0U;

    unused(arg);
    /*
     * Display owns LVGL/ST7789 init, flush tick, and painting. The network task
     * only publishes snapshots so SLE receive/connect callbacks are not slowed
     * by panel I/O.
     */
    if (team_display_init_panel() != 0) {
        osal_printk("[display] st7789 disabled or init failed\r\n");
        return NULL;
    }
    while (1) {
        team_display_read_status(&status);
        if (status.seq != 0U && status.seq != last_status_seq) {
            (void)ws63_st7789_show_status(status.role, status.self, status.online_count,
                status.offline_count, status.event_count, SLE_TEAM_FW_VERSION);
            last_status_seq = status.seq;
        }
        team_display_read_event(&display_event);
        if (display_event.seq != 0U && display_event.seq != last_event_seq) {
            (void)ws63_st7789_show_event(display_event.event, display_event.member,
                display_event.latitude_e6, display_event.longitude_e6, display_event.last_seen_s);
            last_event_seq = display_event.seq;
        }
        ws63_st7789_tick();
        osal_msleep(SLE_TEAM_DISPLAY_LOOP_SLEEP_MS);
    }
    return NULL;
}
static void team_display_spawn_task(void)
{
    osal_task *task = NULL;

    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)team_display_task, NULL, "TeamDisplayTask",
        SLE_TEAM_DISPLAY_TASK_STACK_SIZE);
    if (task != NULL) {
        osal_kthread_set_priority(task, SLE_TEAM_DISPLAY_TASK_PRIO);
    }
    osal_kthread_unlock();
}
static void team_network_spawn_task(void)
{
    osal_task *task = NULL;

    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)team_network_task, NULL, "TeamNetworkTask",
        SLE_TEAM_APP_TASK_STACK_SIZE);
    if (task != NULL) {
        osal_kthread_set_priority(task, SLE_TEAM_APP_TASK_PRIO);
    }
    osal_kthread_unlock();
}
static void team_network_entry(void)
{
    team_display_spawn_task();
    team_network_spawn_task();
}

app_run(team_network_entry);
