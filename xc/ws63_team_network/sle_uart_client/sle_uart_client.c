/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE uart sample of client. \n
 *
 * History: \n
 * 2023-04-03, Create file. \n
 */
#include "common_def.h"
#include "soc_osal.h"
#include "securec.h"
#include "product.h"
#include "bts_le_gap.h"
#include "osal_timer.h"
#include "tcxo.h"

#include "sle_errcode.h"
#include "sle_device_discovery.h"
#include "sle_connection_manager.h"
#include "sle_team_packet.h"
#include "sle_uart_client.h"
#include "sle_uart_server_adv.h"
#define SLE_MTU_SIZE_DEFAULT            520
#define SLE_SEEK_INTERVAL_DEFAULT       100
#define SLE_SEEK_WINDOW_DEFAULT         100
#define UUID_16BIT_LEN                  2
#define UUID_128BIT_LEN                 16
#define SLE_UART_TASK_DELAY_MS          1000
#define SLE_UART_RECV_CNT               1000
#define SLE_UART_LOW_LATENCY_2K         2000
#ifndef SLE_UART_SERVER_NAME
#define SLE_UART_SERVER_NAME            "sle_uart_server"
#endif
#define SLE_UART_CLIENT_LOG             "[sle uart client]"
#define SLE_UART_CLIENT_MAX_CON         8
#define SLE_UART_MEMBER_ID_MAX          254U
#define SLE_UART_BROADCAST_ID           0xFFU
#define SLE_UART_DEFAULT_PROPERTY_HANDLE 2U
#define SLE_UART_RSSI_FAIL_RECOVER_THRESHOLD 3U
#define SLE_UART_RSSI_FAIL_ACTIVITY_GRACE_MS 8000U
#define SLE_UART_SCAN_RESTART_MIN_INTERVAL_MS 1200U
#define SLE_UART_FORCE_RESCAN_MIN_INTERVAL_MS 2500U
#define SLE_UART_SEEK_SAMPLE_LOG_INTERVAL_MS 8000U
#define SLE_UART_CONNECT_SEEK_STOP_TIMEOUT_MS 800U
#define SLE_UART_CONNECT_INFLIGHT_TIMEOUT_MS 6000U

static ssapc_find_service_result_t g_sle_uart_find_service_result = { 0 };
static sle_announce_seek_callbacks_t g_sle_uart_seek_cbk = { 0 };
static ssapc_callbacks_t g_sle_uart_ssapc_cbk = { 0 };
static sle_addr_t g_sle_uart_remote_addr = { 0 };
ssapc_write_param_t g_sle_uart_send_param = { 0 };
static sle_uart_client_seek_filter_cb g_sle_uart_seek_filter = NULL;
static void *g_sle_uart_seek_filter_user_ctx = NULL;

/*
 * One physical SLE ACL link as seen by the local client role.
 *
 * The mesh core works with logical route IDs. This table is the bridge between
 * a route/member ID and the SDK connection ID/property handle used for writes.
 */
typedef struct {
    uint8_t active;
    uint16_t conn_id;
    uint8_t member_id;
    int8_t rssi;
    uint8_t rssi_fail_count;
    uint8_t exchange_requested;
    uint8_t ready;
    uint32_t last_activity_ms;
    uint32_t rssi_probe_hold_start_ms;
    sle_addr_t addr;
} sle_uart_client_conn_t;

static sle_uart_client_conn_t g_sle_uart_conns[SLE_UART_CLIENT_MAX_CON];
static uint8_t g_sle_uart_conn_num = 0;
static uint16_t g_sle_uart_last_conn_id = 0;
static uint8_t g_sle_uart_discovery_ready = 0U;
static uint8_t g_sle_uart_enable_inflight = 0U;
static uint8_t g_sle_uart_enabled = 0U;
static uint8_t g_sle_uart_enable_failed = 0U;
static uint32_t g_sle_uart_scan_start_ms = 0U;
static uint8_t g_sle_uart_seek_active = 0U;
static uint8_t g_sle_uart_seek_stop_for_connect = 0U;
static uint8_t g_sle_uart_connect_inflight = 0U;
static uint8_t g_sle_uart_connect_limit = SLE_UART_CLIENT_MAX_CON;

/* Timestamps below bound scan/connect retries so one bad peer cannot stall discovery. */
static uint32_t g_sle_uart_seek_stop_start_ms = 0U;
static uint32_t g_sle_uart_connect_start_ms = 0U;
static uint32_t g_sle_uart_last_force_rescan_ms = 0U;
static uint32_t g_sle_uart_last_seek_sample_log_ms = 0U;
static uint8_t g_sle_uart_force_rescan_pending = 0U;
static volatile uint8_t g_sle_uart_scan_paused = 0U;

static void sle_uart_client_sample_seek_cbk_register(void);
static void sle_uart_client_sample_ssapc_cbk_register(ssapc_notification_callback notification_cb,
    ssapc_notification_callback indication_cb);

static uint32_t sle_uart_client_now_ms(void)
{
    return (uint32_t)uapi_tcxo_get_ms();
}

static uint8_t sle_uart_client_elapsed_ms(uint32_t now_ms, uint32_t start_ms, uint32_t interval_ms)
{
    if (start_ms == 0U) {
        return 1U;
    }
    return (uint8_t)((uint32_t)(now_ms - start_ms) >= interval_ms);
}

static uint8_t sle_uart_client_should_sample_log(uint32_t now_ms, uint32_t *last_ms, uint32_t interval_ms)
{
    if (last_ms == NULL) {
        return 0U;
    }
    if (*last_ms == 0U || sle_uart_client_elapsed_ms(now_ms, *last_ms, interval_ms) != 0U) {
        *last_ms = now_ms;
        return 1U;
    }
    return 0U;
}

static uint8_t sle_uart_client_effective_connect_limit(void)
{
    /* The mesh role can lower the limit, but never above the SDK slot count. */
    if (g_sle_uart_connect_limit == 0U || g_sle_uart_connect_limit > SLE_UART_CLIENT_MAX_CON) {
        return SLE_UART_CLIENT_MAX_CON;
    }
    return g_sle_uart_connect_limit;
}

static uint8_t sle_uart_client_has_connect_capacity(void)
{
    uint8_t used = g_sle_uart_conn_num;

    /* Reserve a slot for the peer that is between "stop seek" and "connected". */
    if (g_sle_uart_connect_inflight != 0U && used < 0xFFU) {
        used++;
    }
    return used < sle_uart_client_effective_connect_limit() ? 1U : 0U;
}

static void sle_uart_client_clear_connect_inflight(void)
{
    /* Clear the transport-level pending-connect latch after success/failure. */
    g_sle_uart_connect_inflight = 0U;
    g_sle_uart_connect_start_ms = 0U;
}

void sle_uart_client_set_connect_limit(uint8_t max_conns)
{
    uint8_t limit = max_conns;

    if (limit == 0U || limit > SLE_UART_CLIENT_MAX_CON) {
        limit = SLE_UART_CLIENT_MAX_CON;
    }
    if (g_sle_uart_connect_limit == limit) {
        return;
    }
    g_sle_uart_connect_limit = limit;
    osal_printk("%s connect limit=%u hw_max=%u\r\n", SLE_UART_CLIENT_LOG,
        g_sle_uart_connect_limit, SLE_UART_CLIENT_MAX_CON);
}

static uint8_t sle_uart_client_conn_has_recent_activity(const sle_uart_client_conn_t *conn, uint32_t now_ms)
{
    if (conn == NULL || conn->last_activity_ms == 0U) {
        return 0U;
    }
    return (uint8_t)(sle_uart_client_elapsed_ms(now_ms, conn->last_activity_ms,
        SLE_UART_RSSI_FAIL_ACTIVITY_GRACE_MS) == 0U);
}

static uint8_t sle_uart_client_conn_rssi_probe_held(const sle_uart_client_conn_t *conn, uint32_t now_ms)
{
    if (conn == NULL || conn->rssi_probe_hold_start_ms == 0U) {
        return 0U;
    }
    return (uint8_t)(sle_uart_client_elapsed_ms(now_ms, conn->rssi_probe_hold_start_ms,
        SLE_UART_RSSI_FAIL_ACTIVITY_GRACE_MS) == 0U);
}

static uint8_t sle_uart_client_buffer_contains(const uint8_t *buf, size_t buf_len,
    const char *needle, size_t needle_len)
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

static uint8_t sle_uart_client_addr_equal(const sle_addr_t *left, const sle_addr_t *right)
{
    if (left == NULL || right == NULL) {
        return 0U;
    }
    return (uint8_t)(left->type == right->type &&
        memcmp(left->addr, right->addr, sizeof(left->addr)) == 0);
}

static sle_uart_client_conn_t *sle_uart_client_find_conn(uint16_t conn_id)
{
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active != 0U && g_sle_uart_conns[i].conn_id == conn_id) {
            return &g_sle_uart_conns[i];
        }
    }
    return NULL;
}

static sle_uart_client_conn_t *sle_uart_client_find_conn_by_addr(const sle_addr_t *addr)
{
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active != 0U && sle_uart_client_addr_equal(&g_sle_uart_conns[i].addr, addr) != 0U) {
            return &g_sle_uart_conns[i];
        }
    }
    return NULL;
}

static sle_uart_client_conn_t *sle_uart_client_alloc_conn(uint16_t conn_id)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);
    if (conn != NULL) {
        return conn;
    }
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active == 0U) {
            /* New SDK connection IDs are not trusted until SSAP exchange completes. */
            (void)memset_s(&g_sle_uart_conns[i], sizeof(g_sle_uart_conns[i]), 0, sizeof(g_sle_uart_conns[i]));
            g_sle_uart_conns[i].active = 1U;
            g_sle_uart_conns[i].conn_id = conn_id;
            g_sle_uart_conns[i].rssi = SLE_TEAM_RSSI_UNKNOWN;
            g_sle_uart_conns[i].last_activity_ms = sle_uart_client_now_ms();
            g_sle_uart_conn_num++;
            return &g_sle_uart_conns[i];
        }
    }
    return NULL;
}

static void sle_uart_client_remove_conn(uint16_t conn_id)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);
    if (conn == NULL) {
        return;
    }
    (void)memset_s(conn, sizeof(*conn), 0, sizeof(*conn));
    conn->rssi = SLE_TEAM_RSSI_UNKNOWN;
    if (g_sle_uart_conn_num > 0U) {
        g_sle_uart_conn_num--;
    }
    if (g_sle_uart_conn_num == 0U) {
        g_sle_uart_discovery_ready = 0U;
        g_sle_uart_send_param.handle = 0U;
    }
}

uint16_t get_g_sle_uart_conn_id(void)
{
    return g_sle_uart_last_conn_id;
}

uint8_t sle_uart_client_has_conn(uint16_t conn_id)
{
    return sle_uart_client_find_conn(conn_id) != NULL ? 1U : 0U;
}

uint8_t sle_uart_client_is_pending_remote_addr(const sle_addr_t *addr)
{
    if (addr == NULL) {
        return 0U;
    }
    if (g_sle_uart_connect_inflight == 0U && g_sle_uart_seek_stop_for_connect == 0U) {
        return 0U;
    }
    return (uint8_t)(memcmp(&g_sle_uart_remote_addr, addr, sizeof(g_sle_uart_remote_addr)) == 0);
}

uint8_t sle_uart_client_get_active_conns(uint16_t *conn_ids, uint8_t max_conns)
{
    uint8_t count = 0U;

    if (conn_ids == NULL || max_conns == 0U) {
        return 0U;
    }
    for (uint8_t i = 0U; i < SLE_UART_CLIENT_MAX_CON && count < max_conns; i++) {
        if (g_sle_uart_conns[i].active == 0U) {
            continue;
        }
        conn_ids[count++] = g_sle_uart_conns[i].conn_id;
    }
    return count;
}

uint8_t sle_uart_client_get_conn_member(uint16_t conn_id, uint8_t *member_id)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);

    if (conn == NULL || member_id == NULL) {
        return 0U;
    }
    *member_id = conn->member_id;
    return 1U;
}

uint8_t sle_uart_client_disconnect_conn(uint16_t conn_id)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);

    if (conn == NULL) {
        return 0U;
    }
    return sle_disconnect_remote_device(&conn->addr) == ERRCODE_SLE_SUCCESS ? 1U : 0U;
}

void sle_uart_client_set_seek_filter(sle_uart_client_seek_filter_cb seek_filter, void *user_ctx)
{
    g_sle_uart_seek_filter = seek_filter;
    g_sle_uart_seek_filter_user_ctx = user_ctx;
}

uint8_t sle_uart_client_is_ready(void)
{
    return (uint8_t)(g_sle_uart_conn_num != 0U && g_sle_uart_discovery_ready != 0U &&
        g_sle_uart_send_param.handle != 0U);
}

static void sle_uart_client_mark_ready(uint16_t handle, const char *reason)
{
    if (handle == 0U) {
        return;
    }
    g_sle_uart_send_param.handle = handle;
    g_sle_uart_send_param.type = SSAP_PROPERTY_TYPE_VALUE;
    g_sle_uart_discovery_ready = 1U;
    osal_printk("%s discovery ready handle:%u reason:%s\r\n", SLE_UART_CLIENT_LOG,
        handle, reason != NULL ? reason : "unknown");
}

static void sle_uart_client_mark_conn_ready(uint16_t conn_id, const char *reason)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);

    if (conn == NULL) {
        osal_printk("%s conn ready skipped conn_id:%u reason:%s no-conn\r\n",
            SLE_UART_CLIENT_LOG, conn_id, reason != NULL ? reason : "unknown");
        return;
    }
    conn->ready = 1U;
    conn->last_activity_ms = sle_uart_client_now_ms();
    osal_printk("%s conn ready conn_id:%u member:%u reason:%s\r\n",
        SLE_UART_CLIENT_LOG, conn_id, conn->member_id, reason != NULL ? reason : "unknown");
}

static void sle_uart_client_exchange_once(uint16_t conn_id, const char *reason)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);
    ssap_exchange_info_t info = {0};

    if (conn == NULL) {
        osal_printk("%s exchange info skipped conn_id:%u reason:%s no-conn\r\n",
            SLE_UART_CLIENT_LOG, conn_id, reason != NULL ? reason : "unknown");
        return;
    }
    if (conn->exchange_requested != 0U) {
        osal_printk("%s exchange info already requested conn_id:%u reason:%s\r\n",
            SLE_UART_CLIENT_LOG, conn_id, reason != NULL ? reason : "unknown");
        return;
    }
    conn->exchange_requested = 1U;
    info.mtu_size = SLE_MTU_SIZE_DEFAULT;
    info.version = 1;
    osal_printk("%s exchange info request conn_id:%u reason:%s\r\n",
        SLE_UART_CLIENT_LOG, conn_id, reason != NULL ? reason : "unknown");
    ssapc_exchange_info_req(0, conn_id, &info);
}

static void sle_uart_client_remove_pairing(const sle_addr_t *addr, const char *reason)
{
    errcode_t ret;

    if (addr == NULL) {
        return;
    }
    ret = sle_remove_paired_remote_device(addr);
    osal_printk("%s remove paired addr:%02x:**:**:**:%02x:%02x reason:%s ret:0x%x\r\n",
        SLE_UART_CLIENT_LOG,
        addr->addr[0], addr->addr[4], addr->addr[5],
        reason != NULL ? reason : "unknown",
        ret);
}

static uint8_t sle_uart_client_pair_should_reset(errcode_t status)
{
    if (status == ERRCODE_SLE_PAIRING_REJECT ||
        status == ERRCODE_SLE_AUTH_FAIL ||
        status == ERRCODE_SLE_AUTH_PKEY_MISS) {
        return 1U;
    }
    return 0U;
}

static const char *sle_uart_client_pair_status_name(errcode_t status)
{
    if (status == ERRCODE_SLE_SUCCESS) {
        return "success";
    }
    if (status == ERRCODE_SLE_PAIRING_REJECT) {
        return "pairing-reject";
    }
    if (status == ERRCODE_SLE_AUTH_FAIL) {
        return "auth-fail";
    }
    if (status == ERRCODE_SLE_AUTH_PKEY_MISS) {
        return "auth-pkey-miss";
    }
    if (status == ERRCODE_SLE_RMT_DEV_DOWN) {
        return "remote-down";
    }
    return "unknown";
}

uint16_t sle_uart_client_connected_count(void)
{
    return g_sle_uart_conn_num;
}

int8_t sle_uart_client_get_last_rssi(void)
{
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active != 0U) {
            return g_sle_uart_conns[i].rssi;
        }
    }
    return SLE_TEAM_RSSI_UNKNOWN;
}

errcode_t sle_uart_client_read_remote_rssi(void)
{
    uint8_t requested = 0U;
    uint8_t recovered = 0U;
    uint32_t now_ms = sle_uart_client_now_ms();

    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        sle_uart_client_conn_t *conn = &g_sle_uart_conns[i];
        errcode_t ret;

        if (conn->active == 0U) {
            continue;
        }
        if (sle_uart_client_conn_rssi_probe_held(conn, now_ms) != 0U) {
            requested = 1U;
            continue;
        }
        conn->rssi_probe_hold_start_ms = 0U;
        ret = sle_read_remote_device_rssi(conn->conn_id);
        if (ret == ERRCODE_SLE_SUCCESS) {
            conn->rssi_fail_count = 0U;
            conn->last_activity_ms = now_ms;
            requested = 1U;
            continue;
        }
        if (conn->rssi_fail_count < 0xFFU) {
            conn->rssi_fail_count++;
        }
        osal_printk("%s read rssi failed conn_id:%u ret:0x%x fail_count:%u\r\n",
            SLE_UART_CLIENT_LOG, conn->conn_id, ret, conn->rssi_fail_count);
        if (conn->rssi_fail_count >= SLE_UART_RSSI_FAIL_RECOVER_THRESHOLD) {
            /* Packet traffic is stronger liveness evidence than a transient RSSI API failure. */
            if (sle_uart_client_conn_has_recent_activity(conn, now_ms) != 0U) {
                osal_printk("%s keep conn_id:%u member:%u after rssi failures reason:recent-packet\r\n",
                    SLE_UART_CLIENT_LOG, conn->conn_id, conn->member_id);
                conn->rssi_fail_count = 0U;
                conn->rssi_probe_hold_start_ms = now_ms;
                requested = 1U;
                continue;
            }
            osal_printk("%s recover stale conn_id:%u after rssi failures\r\n",
                SLE_UART_CLIENT_LOG, conn->conn_id);
            sle_uart_client_remove_conn(conn->conn_id);
            recovered = 1U;
        }
    }
    if (recovered != 0U) {
        sle_uart_start_scan();
    }
    if (requested == 0U) {
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

void sle_uart_client_force_rescan(void)
{
    errcode_t stop_ret;
    uint32_t now_ms;

    if (g_sle_uart_scan_paused != 0U) {
        return;
    }
    /* Force-rescan is throttled because seek stop/start can pressure BT callbacks. */
    now_ms = sle_uart_client_now_ms();
    if (g_sle_uart_last_force_rescan_ms != 0U &&
        sle_uart_client_elapsed_ms(now_ms, g_sle_uart_last_force_rescan_ms,
        SLE_UART_FORCE_RESCAN_MIN_INTERVAL_MS) == 0U) {
        return;
    }
    g_sle_uart_last_force_rescan_ms = now_ms;
    if (sle_uart_client_has_connect_capacity() == 0U) {
        osal_printk("%s force rescan skipped: conn limit count:%u inflight:%u limit:%u\r\n",
            SLE_UART_CLIENT_LOG, g_sle_uart_conn_num, g_sle_uart_connect_inflight,
            sle_uart_client_effective_connect_limit());
        return;
    }
    g_sle_uart_scan_start_ms = 0U;
    if (g_sle_uart_seek_stop_for_connect != 0U) {
        osal_printk("%s force rescan deferred: pending connect\r\n", SLE_UART_CLIENT_LOG);
        return;
    }
    if (g_sle_uart_seek_active != 0U) {
        g_sle_uart_force_rescan_pending = 1U;
        stop_ret = sle_stop_seek();
        osal_printk("%s force rescan stop seek ret:0x%x\r\n", SLE_UART_CLIENT_LOG, stop_ret);
        if (stop_ret != ERRCODE_SLE_SUCCESS) {
            g_sle_uart_seek_active = 0U;
            g_sle_uart_force_rescan_pending = 0U;
            sle_uart_start_scan();
        }
        return;
    }
    sle_uart_start_scan();
}

void sle_uart_client_pause_scan_request(const char *reason)
{
    /* Used by higher-level role transitions to make scan callbacks quiescent. */
    g_sle_uart_scan_paused = 1U;
    g_sle_uart_force_rescan_pending = 0U;
    g_sle_uart_seek_stop_for_connect = 0U;
    g_sle_uart_seek_stop_start_ms = 0U;
    g_sle_uart_scan_start_ms = 0U;
    osal_printk("%s scan pause requested reason:%s\r\n", SLE_UART_CLIENT_LOG,
        reason != NULL ? reason : "unknown");
}

void sle_uart_client_pause_scan(const char *reason)
{
    errcode_t stop_ret;

    sle_uart_client_pause_scan_request(reason);
    if (g_sle_uart_seek_active != 0U) {
        stop_ret = sle_stop_seek();
        osal_printk("%s scan pause stop seek reason:%s ret:0x%x\r\n", SLE_UART_CLIENT_LOG,
            reason != NULL ? reason : "unknown", stop_ret);
        if (stop_ret != ERRCODE_SLE_SUCCESS) {
            g_sle_uart_seek_active = 0U;
        }
        return;
    }
    osal_printk("%s scan paused reason:%s\r\n", SLE_UART_CLIENT_LOG,
        reason != NULL ? reason : "unknown");
}

void sle_uart_client_resume_scan(const char *reason)
{
    if (g_sle_uart_scan_paused == 0U) {
        return;
    }
    g_sle_uart_scan_paused = 0U;
    g_sle_uart_scan_start_ms = 0U;
    osal_printk("%s scan resumed reason:%s\r\n", SLE_UART_CLIENT_LOG,
        reason != NULL ? reason : "unknown");
}

static void sle_uart_client_connect_pending_remote(const char *reason)
{
    errcode_t ret;

    if (g_sle_uart_scan_paused != 0U) {
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        g_sle_uart_seek_active = 0U;
        g_sle_uart_scan_start_ms = 0U;
        g_sle_uart_force_rescan_pending = 0U;
        osal_printk("%s connect pending skipped: paused reason:%s\r\n", SLE_UART_CLIENT_LOG,
            reason != NULL ? reason : "unknown");
        return;
    }
    if (sle_uart_client_has_connect_capacity() == 0U) {
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        g_sle_uart_seek_active = 0U;
        g_sle_uart_scan_start_ms = 0U;
        g_sle_uart_force_rescan_pending = 0U;
        osal_printk("%s connect pending skipped: conn limit count:%u inflight:%u limit:%u reason:%s\r\n",
            SLE_UART_CLIENT_LOG, g_sle_uart_conn_num, g_sle_uart_connect_inflight,
            sle_uart_client_effective_connect_limit(), reason != NULL ? reason : "unknown");
        return;
    }
    g_sle_uart_seek_stop_for_connect = 0U;
    g_sle_uart_seek_stop_start_ms = 0U;
    g_sle_uart_seek_active = 0U;
    g_sle_uart_scan_start_ms = 0U;
    g_sle_uart_force_rescan_pending = 0U;
    g_sle_uart_connect_inflight = 1U;
    g_sle_uart_connect_start_ms = sle_uart_client_now_ms();
    /* Only one pending address is allowed; later callbacks must match it. */
    ret = sle_connect_remote_device(&g_sle_uart_remote_addr);
    osal_printk("%s connect request addr:%02x:**:**:**:%02x:%02x reason:%s ret:0x%x\r\n",
        SLE_UART_CLIENT_LOG,
        g_sle_uart_remote_addr.addr[0],
        g_sle_uart_remote_addr.addr[4],
        g_sle_uart_remote_addr.addr[5],
        reason != NULL ? reason : "unknown",
        ret);
    if (ret != ERRCODE_SLE_SUCCESS) {
        g_sle_uart_connect_inflight = 0U;
        g_sle_uart_connect_start_ms = 0U;
        sle_uart_start_scan();
    }
}

void sle_uart_client_tick(void)
{
    uint32_t now_ms;

    now_ms = sle_uart_client_now_ms();
    if (g_sle_uart_connect_inflight != 0U && g_sle_uart_connect_start_ms != 0U &&
        sle_uart_client_elapsed_ms(now_ms, g_sle_uart_connect_start_ms,
        SLE_UART_CONNECT_INFLIGHT_TIMEOUT_MS) != 0U) {
        /* Recover from SDK paths that never deliver a matching connect result. */
        osal_printk("%s connect inflight timeout addr:%02x:**:**:**:%02x:%02x count:%u limit:%u\r\n",
            SLE_UART_CLIENT_LOG,
            g_sle_uart_remote_addr.addr[0],
            g_sle_uart_remote_addr.addr[4],
            g_sle_uart_remote_addr.addr[5],
            g_sle_uart_conn_num,
            sle_uart_client_effective_connect_limit());
        (void)sle_disconnect_remote_device(&g_sle_uart_remote_addr);
        g_sle_uart_connect_inflight = 0U;
        g_sle_uart_connect_start_ms = 0U;
        g_sle_uart_scan_start_ms = 0U;
        if (g_sle_uart_scan_paused == 0U) {
            sle_uart_start_scan();
        }
    }
    if (g_sle_uart_seek_stop_for_connect == 0U || g_sle_uart_seek_stop_start_ms == 0U) {
        return;
    }
    if ((now_ms - g_sle_uart_seek_stop_start_ms) < SLE_UART_CONNECT_SEEK_STOP_TIMEOUT_MS) {
        return;
    }
    /* If seek-disable callback is lost, continue with the pending connect anyway. */
    osal_printk("%s seek stop timeout, fallback connect pending addr:%02x:**:**:**:%02x:%02x\r\n",
        SLE_UART_CLIENT_LOG,
        g_sle_uart_remote_addr.addr[0],
        g_sle_uart_remote_addr.addr[4],
        g_sle_uart_remote_addr.addr[5]);
    sle_uart_client_connect_pending_remote("seek-stop-timeout");
}

ssapc_write_param_t *get_g_sle_uart_send_param(void)
{
    return &g_sle_uart_send_param;
}

void sle_uart_start_scan(void)
{
    errcode_t set_ret;
    errcode_t start_ret;
    uint32_t now_ms = sle_uart_client_now_ms();

    if (g_sle_uart_scan_paused != 0U) {
        osal_printk("%s start scan skipped: paused\r\n", SLE_UART_CLIENT_LOG);
        return;
    }
    if (sle_uart_client_has_connect_capacity() == 0U) {
        osal_printk("%s start scan skipped: conn limit count:%u inflight:%u limit:%u\r\n",
            SLE_UART_CLIENT_LOG, g_sle_uart_conn_num, g_sle_uart_connect_inflight,
            sle_uart_client_effective_connect_limit());
        return;
    }
    if (g_sle_uart_seek_active != 0U) {
        osal_printk("%s start scan skipped: seek active conn:%u\r\n", SLE_UART_CLIENT_LOG, g_sle_uart_conn_num);
        return;
    }
    if (g_sle_uart_scan_start_ms != 0U &&
        (now_ms - g_sle_uart_scan_start_ms) < SLE_UART_SCAN_RESTART_MIN_INTERVAL_MS) {
        return;
    }
    /* Duplicate filtering stays off because the mesh may need repeated RSSI samples. */
    g_sle_uart_scan_start_ms = now_ms;
    sle_seek_param_t param = { 0 };
    param.own_addr_type = 0;
    param.filter_duplicates = 0;
    param.seek_filter_policy = 0;
    param.seek_phys = 1;
    param.seek_type[0] = 1;
    param.seek_interval[0] = SLE_SEEK_INTERVAL_DEFAULT;
    param.seek_window[0] = SLE_SEEK_WINDOW_DEFAULT;
    set_ret = sle_set_seek_param(&param);
    start_ret = sle_start_seek();
    if (start_ret == ERRCODE_SLE_SUCCESS) {
        g_sle_uart_seek_active = 1U;
    }
    osal_printk("%s start scan set_ret:0x%x start_ret:0x%x conn:%u\r\n",
        SLE_UART_CLIENT_LOG, set_ret, start_ret, g_sle_uart_conn_num);
}

static void sle_uart_client_sample_sle_enable_cbk(errcode_t status)
{
    osal_printk("sle enable: %d.\r\n", status);
    g_sle_uart_enable_inflight = 0U;
    if (status != ERRCODE_SLE_SUCCESS) {
        g_sle_uart_enable_failed = 1U;
        g_sle_uart_enabled = 0U;
        g_sle_uart_seek_active = 0U;
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        g_sle_uart_force_rescan_pending = 0U;
        osal_printk("%s sle enable callback failed status:0x%x\r\n", SLE_UART_CLIENT_LOG, status);
        return;
    }
    g_sle_uart_enable_failed = 0U;
    g_sle_uart_enabled = 1U;
    g_sle_uart_seek_active = 0U;
    g_sle_uart_seek_stop_for_connect = 0U;
    g_sle_uart_seek_stop_start_ms = 0U;
    g_sle_uart_scan_start_ms = 0U;
    g_sle_uart_force_rescan_pending = 0U;
    sle_uart_client_sample_seek_cbk_register();
    sle_uart_client_sample_ssapc_cbk_register(sle_uart_notification_cb, sle_uart_indication_cb);
    sle_uart_start_scan();
}

static void sle_uart_client_sample_seek_enable_cbk(errcode_t status)
{
    if (status != 0) {
        g_sle_uart_seek_active = 0U;
        osal_printk("%s sle_uart_client_sample_seek_enable_cbk,status error\r\n", SLE_UART_CLIENT_LOG);
    } else {
        g_sle_uart_seek_active = 1U;
        g_sle_uart_scan_start_ms = sle_uart_client_now_ms();
        osal_printk("%s seek enabled\r\n", SLE_UART_CLIENT_LOG);
    }
}

static void sle_uart_client_sample_seek_result_info_cbk(sle_seek_result_info_t *seek_result_data)
{
    static const char server_name[] = SLE_UART_SERVER_NAME;
    size_t server_name_len = sizeof(SLE_UART_SERVER_NAME) - 1U;
    uint8_t matched_name = 0U;
    uint8_t passed_filter = 0U;
    uint8_t should_connect = 0U;
    uint32_t now_ms;

    if (seek_result_data == NULL) {
        osal_printk("status error\r\n");
        return;
    }
    if (g_sle_uart_scan_paused != 0U) {
        return;
    }
    now_ms = sle_uart_client_now_ms();
    g_sle_uart_scan_start_ms = now_ms;
    matched_name = sle_uart_client_buffer_contains(seek_result_data->data, seek_result_data->data_length,
        server_name, server_name_len);
    /*
     * The app-level filter checks route ID, team policy, and firmware fingerprint
     * before transport spends a connection slot.
     */
    if (g_sle_uart_seek_filter != NULL) {
        passed_filter = g_sle_uart_seek_filter(seek_result_data, g_sle_uart_seek_filter_user_ctx);
    } else {
        passed_filter = 1U;
    }
    should_connect = (uint8_t)(sle_uart_client_has_connect_capacity() != 0U &&
        matched_name != 0U && passed_filter != 0U && g_sle_uart_seek_stop_for_connect == 0U &&
        sle_uart_client_find_conn_by_addr(&seek_result_data->addr) == NULL);
    if (should_connect != 0U ||
        sle_uart_client_should_sample_log(now_ms, &g_sle_uart_last_seek_sample_log_ms,
        SLE_UART_SEEK_SAMPLE_LOG_INTERVAL_MS) != 0U) {
        osal_printk("%s seek result len:%u rssi:%d name:%u filter:%u addr:%02x:**:**:**:%02x:%02x\r\n",
            SLE_UART_CLIENT_LOG,
            seek_result_data->data_length,
            seek_result_data->rssi,
            matched_name,
            passed_filter,
            seek_result_data->addr.addr[0],
            seek_result_data->addr.addr[4],
            seek_result_data->addr.addr[5]);
    }
    if (should_connect != 0U) {
        errcode_t stop_ret;
        osal_printk("%s will connect addr:%02x:**:**:**:%02x:%02x count:%u inflight:%u limit:%u\r\n", SLE_UART_CLIENT_LOG,
            seek_result_data->addr.addr[0], seek_result_data->addr.addr[4],
            seek_result_data->addr.addr[5], g_sle_uart_conn_num, g_sle_uart_connect_inflight,
            sle_uart_client_effective_connect_limit());
        if (g_sle_uart_scan_paused != 0U) {
            return;
        }
        memcpy_s(&g_sle_uart_remote_addr, sizeof(sle_addr_t), &seek_result_data->addr, sizeof(sle_addr_t));
        /* The SDK expects seek to stop before a connect request is issued. */
        g_sle_uart_seek_stop_for_connect = 1U;
        g_sle_uart_seek_stop_start_ms = sle_uart_client_now_ms();
        if (g_sle_uart_scan_paused != 0U) {
            g_sle_uart_seek_stop_for_connect = 0U;
            g_sle_uart_seek_stop_start_ms = 0U;
            return;
        }
        stop_ret = sle_stop_seek();
        osal_printk("%s stop seek for connect ret:0x%x\r\n", SLE_UART_CLIENT_LOG, stop_ret);
        if (stop_ret != ERRCODE_SLE_SUCCESS) {
            sle_uart_client_connect_pending_remote("stop-seek-fail");
        }
    }
}

static void sle_uart_client_sample_seek_disable_cbk(errcode_t status)
{
    uint8_t do_connect = g_sle_uart_seek_stop_for_connect;

    /* Seek-disable is the normal handoff point from scanning to connecting. */
    g_sle_uart_seek_active = 0U;
    g_sle_uart_scan_start_ms = 0U;
    if (g_sle_uart_scan_paused != 0U) {
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        g_sle_uart_force_rescan_pending = 0U;
        osal_printk("%s seek disabled while paused status:%x\r\n", SLE_UART_CLIENT_LOG, status);
        return;
    }
    if (status != 0) {
        osal_printk("%s sle_uart_client_sample_seek_disable_cbk,status error = %x\r\n", SLE_UART_CLIENT_LOG, status);
        if (do_connect != 0U) {
            sle_uart_client_connect_pending_remote("seek-disable-error");
        } else if (g_sle_uart_force_rescan_pending != 0U) {
            g_sle_uart_force_rescan_pending = 0U;
            g_sle_uart_seek_stop_for_connect = 0U;
            g_sle_uart_seek_stop_start_ms = 0U;
            osal_printk("%s seek disable error, retry force rescan\r\n", SLE_UART_CLIENT_LOG);
            sle_uart_start_scan();
        }
    } else if (do_connect != 0U) {
        sle_uart_client_connect_pending_remote("seek-disable-cbk");
    } else if (g_sle_uart_force_rescan_pending != 0U) {
        g_sle_uart_force_rescan_pending = 0U;
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        osal_printk("%s seek disabled for force rescan\r\n", SLE_UART_CLIENT_LOG);
        sle_uart_start_scan();
    } else {
        g_sle_uart_seek_stop_for_connect = 0U;
        g_sle_uart_seek_stop_start_ms = 0U;
        osal_printk("%s seek disabled without connect target\r\n", SLE_UART_CLIENT_LOG);
    }
}

static void sle_uart_client_sample_seek_cbk_register(void)
{
    errcode_t ret;

    /* Merge with server-side announce callbacks; both roles share the SDK callback table. */
    g_sle_uart_seek_cbk.sle_enable_cb = sle_uart_client_sample_sle_enable_cbk;
    g_sle_uart_seek_cbk.seek_enable_cb = sle_uart_client_sample_seek_enable_cbk;
    g_sle_uart_seek_cbk.seek_result_cb = sle_uart_client_sample_seek_result_info_cbk;
    g_sle_uart_seek_cbk.seek_disable_cb = sle_uart_client_sample_seek_disable_cbk;
    ret = sle_uart_announce_seek_merge_cbks(&g_sle_uart_seek_cbk);
    if (ret != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s seek cb register fail ret:0x%x\r\n", SLE_UART_CLIENT_LOG, ret);
    }
}

void sle_uart_client_handle_connect_state_changed(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    sle_addr_t disconnect_addr = {0};
    const sle_addr_t *disconnect_addr_ptr = addr;
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);
    uint8_t was_connect_inflight = g_sle_uart_connect_inflight;

    osal_printk("%s conn state changed disc_reason:0x%x\r\n", SLE_UART_CLIENT_LOG, disc_reason);
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        /* Connected means ACL is up; route binding still waits for mesh packets. */
        conn = sle_uart_client_alloc_conn(conn_id);
        sle_uart_client_clear_connect_inflight();
        osal_printk("%s SLE_ACB_STATE_CONNECTED\r\n", SLE_UART_CLIENT_LOG);
        if (conn == NULL) {
            if (addr != NULL) {
                (void)sle_disconnect_remote_device(addr);
            }
            return;
        }
        if (addr != NULL) {
            (void)memcpy_s(&conn->addr, sizeof(conn->addr), addr, sizeof(*addr));
        }
        g_sle_uart_last_conn_id = conn_id;
        g_sle_uart_scan_start_ms = 0U;
        (void)sle_read_remote_device_rssi(conn_id);
        /*
         * WS63 v4 runtime-role profile uses plain SLE ACL + SSAP exchange.
         * For this profile, forcing security pairing can trigger persistent
         * pairing-reject/disconnect loops on some boards after reboot.
         */
        if (pair_state == SLE_PAIR_NONE) {
            osal_printk("%s pair skip conn_id:%u pair_state:none\r\n", SLE_UART_CLIENT_LOG, conn_id);
        }
        if (pair_state == SLE_PAIR_PAIRED) {
            sle_uart_client_exchange_once(conn_id, "connect-paired");
        } else {
            sle_uart_client_exchange_once(conn_id, "connect-unpaired");
        }
        if (sle_uart_client_has_connect_capacity() != 0U) {
            osal_printk("%s continue seek after connect count:%u limit:%u\r\n",
                SLE_UART_CLIENT_LOG, g_sle_uart_conn_num, sle_uart_client_effective_connect_limit());
            sle_uart_start_scan();
        } else {
            osal_printk("%s keep seek stopped after connect count:%u limit:%u\r\n",
                SLE_UART_CLIENT_LOG, g_sle_uart_conn_num, sle_uart_client_effective_connect_limit());
        }
    } else if (conn_state == SLE_ACB_STATE_NONE) {
        osal_printk("%s SLE_ACB_STATE_NONE\r\n", SLE_UART_CLIENT_LOG);
        sle_uart_client_clear_connect_inflight();
        if (was_connect_inflight != 0U && addr != NULL &&
            memcmp(&g_sle_uart_remote_addr, addr, sizeof(g_sle_uart_remote_addr)) == 0) {
            /* A failed pending connect frees the reserved slot immediately. */
            sle_uart_start_scan();
            return;
        }
        if (conn != NULL && addr != NULL && sle_uart_client_addr_equal(&conn->addr, addr) == 0U) {
            /* Ignore stale events for an address that no longer owns this conn_id. */
            osal_printk("%s ignore state-none conn_id:%u active:%02x:**:**:**:%02x:%02x "
                "event:%02x:**:**:**:%02x:%02x\r\n",
                SLE_UART_CLIENT_LOG,
                conn_id,
                conn->addr.addr[0], conn->addr.addr[4], conn->addr.addr[5],
                addr->addr[0], addr->addr[4], addr->addr[5]);
            sle_uart_start_scan();
            return;
        }
        sle_uart_client_remove_conn(conn_id);
        sle_uart_start_scan();
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        osal_printk("%s SLE_ACB_STATE_DISCONNECTED\r\n", SLE_UART_CLIENT_LOG);
        sle_uart_client_clear_connect_inflight();
        if (was_connect_inflight != 0U && addr != NULL &&
            memcmp(&g_sle_uart_remote_addr, addr, sizeof(g_sle_uart_remote_addr)) == 0) {
            /* Disconnect for the pending address is treated as a connect failure. */
            sle_uart_start_scan();
            return;
        }
        if (disconnect_addr_ptr == NULL) {
            if (conn != NULL) {
                (void)memcpy_s(&disconnect_addr, sizeof(disconnect_addr), &conn->addr, sizeof(conn->addr));
                disconnect_addr_ptr = &disconnect_addr;
            }
        }
        if (conn != NULL && disconnect_addr_ptr != NULL &&
            sle_uart_client_addr_equal(&conn->addr, disconnect_addr_ptr) == 0U) {
            /* A stale disconnect must not tear down the active parent/child link. */
            osal_printk("%s ignore disconnect conn_id:%u active:%02x:**:**:**:%02x:%02x "
                "event:%02x:**:**:**:%02x:%02x\r\n",
                SLE_UART_CLIENT_LOG,
                conn_id,
                conn->addr.addr[0], conn->addr.addr[4], conn->addr.addr[5],
                disconnect_addr_ptr->addr[0], disconnect_addr_ptr->addr[4], disconnect_addr_ptr->addr[5]);
            sle_uart_start_scan();
            return;
        }
        sle_uart_client_remove_conn(conn_id);
        sle_uart_start_scan();
    } else {
        osal_printk("%s status error\r\n", SLE_UART_CLIENT_LOG);
    }
}

void sle_uart_client_handle_read_rssi(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);

    if (status == ERRCODE_SLE_SUCCESS && conn != NULL) {
        conn->rssi = rssi;
        conn->rssi_fail_count = 0U;
        conn->last_activity_ms = sle_uart_client_now_ms();
        conn->rssi_probe_hold_start_ms = 0U;
        osal_printk("%s rssi conn_id:%d rssi:%d\r\n", SLE_UART_CLIENT_LOG, conn_id, rssi);
        return;
    }
    if (conn != NULL && conn->rssi_fail_count < 0xFFU) {
        conn->rssi_fail_count++;
    }
}

void sle_uart_client_handle_pair_complete(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    const sle_addr_t *pair_addr = addr != NULL ? addr : &g_sle_uart_remote_addr;

    if (addr != NULL) {
        osal_printk("%s pair complete conn_id:%d status:0x%x(%s), addr:%02x***%02x%02x\n", SLE_UART_CLIENT_LOG,
                    conn_id, status, sle_uart_client_pair_status_name(status), addr->addr[0], addr->addr[4],
                    addr->addr[5]);
    } else {
        osal_printk("%s pair complete conn_id:%d status:0x%x(%s), addr:null\n", SLE_UART_CLIENT_LOG,
            conn_id, status, sle_uart_client_pair_status_name(status));
    }
    if (status == 0) {
        sle_uart_client_exchange_once(conn_id, "pair-complete");
    } else if (sle_uart_client_pair_should_reset(status) != 0U) {
        sle_uart_client_remove_pairing(pair_addr, "pair-complete-fail");
    }
}

static void sle_uart_client_sample_exchange_info_cbk(uint8_t client_id, uint16_t conn_id, ssap_exchange_info_t *param,
                                                     errcode_t status)
{
    osal_printk("%s exchange_info_cbk,pair complete client id:%d status:%d\r\n",
                SLE_UART_CLIENT_LOG, client_id, status);
    if (param != NULL) {
        osal_printk("%s exchange mtu, mtu size: %d, version: %d.\r\n", SLE_UART_CLIENT_LOG,
                    param->mtu_size, param->version);
    }
    if (status == ERRCODE_SLE_SUCCESS) {
        /*
         * Both peers run this fixed WS63 SLE UART profile. Avoid rejoin loops caused by
         * transient property discovery failures after one side reboots.
         */
        sle_uart_client_mark_ready(SLE_UART_DEFAULT_PROPERTY_HANDLE, "fixed-profile");
        sle_uart_client_mark_conn_ready(conn_id, "exchange-info");
    }
}

static void sle_uart_client_sample_find_structure_cbk(uint8_t client_id, uint16_t conn_id,
                                                      ssapc_find_service_result_t *service,
                                                      errcode_t status)
{
    osal_printk("%s find structure cbk client: %d conn_id:%d status: %d \r\n", SLE_UART_CLIENT_LOG,
                client_id, conn_id, status);
    if (service == NULL || status != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s find structure failed client:%d conn:%d status:%d\r\n",
            SLE_UART_CLIENT_LOG, client_id, conn_id, status);
        return;
    }
    osal_printk("%s find structure start_hdl:[0x%02x], end_hdl:[0x%02x], uuid len:%d\r\n", SLE_UART_CLIENT_LOG,
                service->start_hdl, service->end_hdl, service->uuid.len);
    g_sle_uart_find_service_result.start_hdl = service->start_hdl;
    g_sle_uart_find_service_result.end_hdl = service->end_hdl;
    memcpy_s(&g_sle_uart_find_service_result.uuid, sizeof(sle_uuid_t), &service->uuid, sizeof(sle_uuid_t));
}

static void sle_uart_client_sample_find_property_cbk(uint8_t client_id, uint16_t conn_id,
                                                     ssapc_find_property_result_t *property, errcode_t status)
{
    if (property == NULL || status != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s find property failed client:%d conn:%d status:%d\r\n",
                    SLE_UART_CLIENT_LOG, client_id, conn_id, status);
        return;
    }
    osal_printk("%s sle_uart_client_sample_find_property_cbk, client id: %d, conn id: %d, operate ind: %d, "
                "descriptors count: %d status:%d property->handle %d\r\n", SLE_UART_CLIENT_LOG,
                client_id, conn_id, property->operate_indication,
                property->descriptors_count, status, property->handle);
    /* Dynamic discovery is supported, but the fixed profile fallback is enough for this mesh. */
    sle_uart_client_mark_ready(property->handle, "property-discovery");
    sle_uart_client_mark_conn_ready(conn_id, "property-discovery");
}

static void sle_uart_client_sample_find_structure_cmp_cbk(uint8_t client_id, uint16_t conn_id,
                                                          ssapc_find_structure_result_t *structure_result,
                                                          errcode_t status)
{
    unused(conn_id);
    if (structure_result == NULL || status != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s find structure cmp failed client:%d status:%d\r\n",
            SLE_UART_CLIENT_LOG, client_id, status);
        return;
    }
    osal_printk("%s sle_uart_client_sample_find_structure_cmp_cbk,client id:%d status:%d type:%d uuid len:%d \r\n",
                SLE_UART_CLIENT_LOG, client_id, status, structure_result->type, structure_result->uuid.len);
}

static void sle_uart_client_sample_write_cfm_cb(uint8_t client_id, uint16_t conn_id,
                                                ssapc_write_result_t *write_result, errcode_t status)
{
    if (write_result == NULL || status != ERRCODE_SLE_SUCCESS) {
        osal_printk("%s write cfm failed conn_id:%d client id:%d status:%d\r\n",
            SLE_UART_CLIENT_LOG, conn_id, client_id, status);
        return;
    }
    osal_printk("%s sle_uart_client_sample_write_cfm_cb, conn_id:%d client id:%d status:%d handle:%02x type:%02x\r\n",
                SLE_UART_CLIENT_LOG, conn_id, client_id, status, write_result->handle, write_result->type);
}

static void sle_uart_client_sample_ssapc_cbk_register(ssapc_notification_callback notification_cb,
                                                      ssapc_notification_callback indication_cb)
{
    /* SSAP callbacks deliver mesh packets upward and write confirmations downward. */
    g_sle_uart_ssapc_cbk.exchange_info_cb = sle_uart_client_sample_exchange_info_cbk;
    g_sle_uart_ssapc_cbk.find_structure_cb = sle_uart_client_sample_find_structure_cbk;
    g_sle_uart_ssapc_cbk.ssapc_find_property_cbk = sle_uart_client_sample_find_property_cbk;
    g_sle_uart_ssapc_cbk.find_structure_cmp_cb = sle_uart_client_sample_find_structure_cmp_cbk;
    g_sle_uart_ssapc_cbk.write_cfm_cb = sle_uart_client_sample_write_cfm_cb;
    g_sle_uart_ssapc_cbk.notification_cb = notification_cb;
    g_sle_uart_ssapc_cbk.indication_cb = indication_cb;
    ssapc_register_callbacks(&g_sle_uart_ssapc_cbk);
}


void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb)
{
    errcode_t ret;

    /* Client init is non-blocking; enable callback will start scanning when ready. */
    sle_uart_client_sample_seek_cbk_register();
    sle_uart_client_sample_ssapc_cbk_register(notification_cb, indication_cb);
    g_sle_uart_enable_inflight = 1U;
    osal_printk("[SLE Client] try enable.\r\n");
    ret = enable_sle();
    if (ret != ERRCODE_SUCC) {
        g_sle_uart_enable_inflight = 0U;
        if (g_sle_uart_enabled == 0U) {
            g_sle_uart_enable_failed = 1U;
        }
        osal_printk("[SLE Client] sle enable call failed ret=0x%x, continue if already enabled.\r\n", ret);
        sle_uart_start_scan();
    }
}

errcode_t sle_uart_client_send_by_conn(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    sle_uart_client_conn_t *conn = sle_uart_client_find_conn(conn_id);
    ssapc_write_param_t *param = get_g_sle_uart_send_param();

    if (param == NULL || data == NULL || len == 0U || g_sle_uart_discovery_ready == 0U ||
        conn == NULL || conn->ready == 0U) {
        if (conn != NULL && conn->exchange_requested == 0U) {
            /* Lazy recovery: a send attempt can kick SSAP exchange if callbacks lagged. */
            sle_uart_client_exchange_once(conn_id, "send-not-ready");
        }
        return ERRCODE_SLE_FAIL;
    }
    param->data_len = len;
    param->data = (uint8_t *)data;
    return ssapc_write_req(0, conn_id, param);
}

errcode_t sle_uart_client_send_all(const uint8_t *data, uint16_t len)
{
    uint8_t sent = 0U;
    errcode_t first_fail = ERRCODE_SLE_FAIL;

    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active == 0U) {
            continue;
        }
        errcode_t ret = sle_uart_client_send_by_conn(g_sle_uart_conns[i].conn_id, data, len);
        if (ret == ERRCODE_SLE_SUCCESS) {
            sent = 1U;
        } else if (first_fail == ERRCODE_SLE_FAIL) {
            first_fail = ret;
        }
    }
    return sent != 0U ? ERRCODE_SLE_SUCCESS : first_fail;
}

uint8_t sle_uart_client_bind_member_conn(uint8_t member_id, uint16_t conn_id)
{
    sle_uart_client_conn_t *conn;

    if (member_id == 0U || member_id > SLE_UART_MEMBER_ID_MAX) {
        return 0U;
    }
    conn = sle_uart_client_find_conn(conn_id);
    if (conn == NULL) {
        conn = sle_uart_client_alloc_conn(conn_id);
        if (conn == NULL) {
            osal_printk("%s bind member:%u conn_id:%u failed: table full\r\n",
                SLE_UART_CLIENT_LOG, member_id, conn_id);
            return 0U;
        }
        if (g_sle_uart_discovery_ready == 0U || g_sle_uart_send_param.handle == 0U) {
            sle_uart_client_mark_ready(SLE_UART_DEFAULT_PROPERTY_HANDLE, "packet-bind-recover");
        }
        osal_printk("%s recover conn_id:%u from packet bind member:%u\r\n",
            SLE_UART_CLIENT_LOG, conn_id, member_id);
    }
    /* Binding is how the app maps a logical route/member to this physical link. */
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active != 0U && g_sle_uart_conns[i].conn_id != conn_id &&
            g_sle_uart_conns[i].member_id == member_id) {
            g_sle_uart_conns[i].member_id = 0U;
        }
    }
    conn->member_id = member_id;
    conn->last_activity_ms = sle_uart_client_now_ms();
    conn->rssi_fail_count = 0U;
    osal_printk("%s bind member:%u conn_id:%u\r\n", SLE_UART_CLIENT_LOG, member_id, conn_id);
    return 1U;
}

uint8_t sle_uart_client_find_conn_by_member(uint8_t member_id, uint16_t *conn_id)
{
    if (member_id == 0U || member_id == SLE_UART_BROADCAST_ID || conn_id == NULL) {
        return 0U;
    }
    for (uint8_t i = 0; i < SLE_UART_CLIENT_MAX_CON; i++) {
        if (g_sle_uart_conns[i].active != 0U && g_sle_uart_conns[i].member_id == member_id) {
            *conn_id = g_sle_uart_conns[i].conn_id;
            return 1U;
        }
    }
    return 0U;
}
