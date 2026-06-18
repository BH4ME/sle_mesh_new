/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: sle adv config for sle uart server. \n
 *
 * History: \n
 * 2023-07-17, Create file. \n
 */
#include "securec.h"
#include "errcode.h"
#include "osal_addr.h"
#include "product.h"
#include "sle_common.h"
#include "sle_uart_server.h"
#include "sle_device_discovery.h"
#include "sle_errcode.h"
#include "osal_debug.h"
#include "osal_task.h"
#include "string.h"
#include "sle_uart_server_adv.h"

/* sle device name */
#define NAME_MAX_LENGTH 16
/* 连接调度间隔12.5ms，单位125us */
#define SLE_CONN_INTV_MIN_DEFAULT                 0x64
/* 连接调度间隔12.5ms，单位125us */
#define SLE_CONN_INTV_MAX_DEFAULT                 0x64
/* 连接调度间隔25ms，单位125us */
#define SLE_ADV_INTERVAL_MIN_DEFAULT              0xC8
/* 连接调度间隔25ms，单位125us */
#define SLE_ADV_INTERVAL_MAX_DEFAULT              0xC8
/* 超时时间2500ms，单位10ms */
#define SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT      0xFA
/* 延迟周期（slot），收紧为 32 以降低断链检测与恢复抖动 */
#define SLE_CONN_MAX_LATENCY                      0x20
/* Broadcast TX power in dBm. Keep scan response declaration aligned. */
#define SLE_ADV_TX_POWER_DBM  18
/* 广播ID */
#define SLE_ADV_HANDLE_DEFAULT                    1
/* 最大广播数据长度 */
#define SLE_ADV_DATA_LEN_MAX                      251
/* 广播名称 */
static uint8_t sle_local_name[NAME_MAX_LENGTH] = "sle_uart_server";
static uint8_t g_sle_uart_local_addr[SLE_ADDR_LEN] = { 0x01, 0x02, 0x03, 0x04, 0x05, 0x06 };
static uint8_t g_sle_uart_route_id = 0U;
static uint16_t g_sle_uart_fw_compat = SLE_TEAM_ADV_FW_COMPAT_ANY;
static sle_announce_seek_callbacks_t g_sle_uart_announce_seek_cbks = {0};

/*
 * Advertising payload for the fixed WS63 mesh profile.
 *
 * The scanner uses this metadata to find the server, identify the route ID,
 * and reject incompatible firmware before spending connection attempts.
 */
#define SLE_SERVER_INIT_DELAY_MS    1000
#define sample_at_log_print(fmt, args...) osal_printk(fmt, ##args)
#define SLE_UART_SERVER_LOG "[sle uart server]"

void sle_uart_server_adv_set_local_addr(const uint8_t addr[SLE_ADDR_LEN])
{
    if (addr == NULL) {
        return;
    }
    /* Seed the advertiser's own address so peers can report a stable route label. */
    (void)memcpy_s(g_sle_uart_local_addr, sizeof(g_sle_uart_local_addr), addr, SLE_ADDR_LEN);
}

void sle_uart_server_adv_set_route_id(uint8_t route_id)
{
    if (route_id == 0U || route_id == 0xFFU) {
        g_sle_uart_route_id = 0U;
        return;
    }
    /* Route ID is carried in advertising so the scanner can pre-label peers. */
    g_sle_uart_route_id = route_id;
}

void sle_uart_server_adv_set_fw_compat(uint16_t fw_compat)
{
    /* 16-bit compatibility fingerprint is the same-firmware gate advertised on-air. */
    g_sle_uart_fw_compat = fw_compat;
}

static uint16_t sle_set_adv_local_name(uint8_t *adv_data, uint16_t max_len)
{
    errno_t ret;
    uint8_t index = 0;

    uint8_t *local_name = sle_local_name;
    uint8_t local_name_len = sizeof(sle_local_name) - 1;
    /* Local name remains short because the route/fingerprint fields matter more. */
    sample_at_log_print("%s local_name_len = %d\r\n", SLE_UART_SERVER_LOG, local_name_len);
    sample_at_log_print("%s local_name: ", SLE_UART_SERVER_LOG);
    for (uint8_t i = 0; i < local_name_len; i++) {
        sample_at_log_print("0x%02x ", local_name[i]);
    }
    sample_at_log_print("\r\n");
    adv_data[index++] = local_name_len + 1;
    adv_data[index++] = SLE_ADV_DATA_TYPE_COMPLETE_LOCAL_NAME;
    ret = memcpy_s(&adv_data[index], max_len - index, local_name, local_name_len);
    if (ret != EOK) {
        sample_at_log_print("%s memcpy fail\r\n", SLE_UART_SERVER_LOG);
        return 0;
    }
    return (uint16_t)index + local_name_len;
}

static uint16_t sle_set_adv_route_hint(uint8_t *adv_data, uint16_t max_len)
{
    if (adv_data == NULL || max_len < 7U || g_sle_uart_route_id == 0U) {
        return 0U;
    }
    /* Manufacturer data carries the route ID and proof-line fingerprint. */
    adv_data[0] = 6U;
    adv_data[1] = SLE_ADV_DATA_TYPE_MANUFACTURER_SPECIFIC_DATA;
    adv_data[2] = SLE_TEAM_ADV_ROUTE_MAGIC_0;
    adv_data[3] = SLE_TEAM_ADV_ROUTE_MAGIC_1;
    adv_data[4] = g_sle_uart_route_id;
    adv_data[5] = (uint8_t)(g_sle_uart_fw_compat & 0xFFU);
    adv_data[6] = (uint8_t)((g_sle_uart_fw_compat >> 8U) & 0xFFU);
    return 7U;
}

static uint16_t sle_set_adv_data(uint8_t *adv_data)
{
    size_t len = 0;
    uint16_t idx = 0;
    errno_t  ret = 0;

    len = sizeof(struct sle_adv_common_value);
    struct sle_adv_common_value adv_disc_level = {
        .length = len - 1,
        .type = SLE_ADV_DATA_TYPE_DISCOVERY_LEVEL,
        .value = SLE_ANNOUNCE_LEVEL_NORMAL,
    };
    /* Announce as a normal discoverable device. */
    ret = memcpy_s(&adv_data[idx], SLE_ADV_DATA_LEN_MAX - idx, &adv_disc_level, len);
    if (ret != EOK) {
        sample_at_log_print("%s adv_disc_level memcpy fail\r\n", SLE_UART_SERVER_LOG);
        return 0;
    }
    idx += len;

    len = sizeof(struct sle_adv_common_value);
    struct sle_adv_common_value adv_access_mode = {
        .length = len - 1,
        .type = SLE_ADV_DATA_TYPE_ACCESS_MODE,
        .value = 0,
    };
    /* Leave access mode open so the mesh can build connections directly. */
    ret = memcpy_s(&adv_data[idx], SLE_ADV_DATA_LEN_MAX - idx, &adv_access_mode, len);
    if (ret != EOK) {
        sample_at_log_print("%s adv_access_mode memcpy fail\r\n", SLE_UART_SERVER_LOG);
        return 0;
    }
    idx += len;

    return idx;
}

static uint16_t sle_set_scan_response_data(uint8_t *scan_rsp_data)
{
    uint16_t idx = 0;
    errno_t ret;
    size_t scan_rsp_data_len = sizeof(struct sle_adv_common_value);

    struct sle_adv_common_value tx_power_level = {
        .length = scan_rsp_data_len - 1,
        .type = SLE_ADV_DATA_TYPE_TX_POWER_LEVEL,
        .value = SLE_ADV_TX_POWER_DBM,
    };
    /* Scan response adds TX power, route hint, and name. */
    ret = memcpy_s(scan_rsp_data, SLE_ADV_DATA_LEN_MAX, &tx_power_level, scan_rsp_data_len);
    if (ret != EOK) {
        sample_at_log_print("%s sle scan response data memcpy fail\r\n", SLE_UART_SERVER_LOG);
        return 0;
    }
    idx += scan_rsp_data_len;

    idx += sle_set_adv_route_hint(&scan_rsp_data[idx], SLE_ADV_DATA_LEN_MAX - idx);

    /* set local name */
    idx += sle_set_adv_local_name(&scan_rsp_data[idx], SLE_ADV_DATA_LEN_MAX - idx);
    return idx;
}

static int sle_set_default_announce_param(void)
{
    errno_t ret;
    sle_announce_param_t param = {0};
    uint8_t index;
    param.announce_mode = SLE_ANNOUNCE_MODE_CONNECTABLE_SCANABLE;
    param.announce_handle = SLE_ADV_HANDLE_DEFAULT;
    param.announce_gt_role = SLE_ANNOUNCE_ROLE_T_CAN_NEGO;
    param.announce_level = SLE_ANNOUNCE_LEVEL_NORMAL;
    param.announce_channel_map = SLE_ADV_CHANNEL_MAP_DEFAULT;
    param.announce_interval_min = SLE_ADV_INTERVAL_MIN_DEFAULT;
    param.announce_interval_max = SLE_ADV_INTERVAL_MAX_DEFAULT;
    param.conn_interval_min = SLE_CONN_INTV_MIN_DEFAULT;
    param.conn_interval_max = SLE_CONN_INTV_MAX_DEFAULT;
    param.conn_max_latency = SLE_CONN_MAX_LATENCY;
    param.conn_supervision_timeout = SLE_CONN_SUPERVISION_TIMEOUT_DEFAULT;
    param.announce_tx_power = SLE_ADV_TX_POWER_DBM;
    param.own_addr.type = 0;
    /* Own address is copied into the announce params so the radio can advertise it. */
    ret = memcpy_s(param.own_addr.addr, SLE_ADDR_LEN, g_sle_uart_local_addr, SLE_ADDR_LEN);
    if (ret != EOK) {
        sample_at_log_print("%s sle_set_default_announce_param data memcpy fail\r\n", SLE_UART_SERVER_LOG);
        return 0;
    }
    sample_at_log_print("%s sle_uart_local addr: ", SLE_UART_SERVER_LOG);
    for (index = 0; index < SLE_ADDR_LEN; index++) {
        sample_at_log_print("0x%02x ", param.own_addr.addr[index]);
    }
    sample_at_log_print("\r\n");
    return sle_set_announce_param(param.announce_handle, &param);
}

static int sle_set_default_announce_data(void)
{
    errcode_t ret;
    uint8_t announce_data_len = 0;
    uint8_t seek_data_len = 0;
    sle_announce_data_t data = {0};
    uint8_t adv_handle = SLE_ADV_HANDLE_DEFAULT;
    uint8_t announce_data[SLE_ADV_DATA_LEN_MAX] = {0};
    uint8_t seek_rsp_data[SLE_ADV_DATA_LEN_MAX] = {0};
    uint8_t data_index = 0;

    announce_data_len = sle_set_adv_data(announce_data);
    data.announce_data = announce_data;
    data.announce_data_len = announce_data_len;

    /* The scan response is where peers learn the route hint and local name. */
    sample_at_log_print("%s data.announce_data_len = %d\r\n", SLE_UART_SERVER_LOG, data.announce_data_len);
    sample_at_log_print("%s data.announce_data: ", SLE_UART_SERVER_LOG);
    for (data_index = 0; data_index<data.announce_data_len; data_index++) {
        sample_at_log_print("0x%02x ", data.announce_data[data_index]);
    }
    sample_at_log_print("\r\n");

    seek_data_len = sle_set_scan_response_data(seek_rsp_data);
    data.seek_rsp_data = seek_rsp_data;
    data.seek_rsp_data_len = seek_data_len;

    sample_at_log_print("%s data.seek_rsp_data_len = %d\r\n", SLE_UART_SERVER_LOG, data.seek_rsp_data_len);
    sample_at_log_print("%s data.seek_rsp_data: ", SLE_UART_SERVER_LOG);
    for (data_index = 0; data_index<data.seek_rsp_data_len; data_index++) {
        sample_at_log_print("0x%02x ", data.seek_rsp_data[data_index]);
    }
    sample_at_log_print("\r\n");

    ret = sle_set_announce_data(adv_handle, &data);
    if (ret == ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s set announce data success.\r\n", SLE_UART_SERVER_LOG);
    } else {
        sample_at_log_print("%s set adv param fail.\r\n", SLE_UART_SERVER_LOG);
    }
    return ERRCODE_SLE_SUCCESS;
}

static void sle_announce_enable_cbk(uint32_t announce_id, errcode_t status)
{
    /* Advertise enable/disable callbacks are mostly for bring-up diagnostics. */
    sample_at_log_print("%s sle announce enable callback id:%02x, state:%x\r\n", SLE_UART_SERVER_LOG, announce_id,
        status);
}

static void sle_announce_disable_cbk(uint32_t announce_id, errcode_t status)
{
    sample_at_log_print("%s sle announce disable callback id:%02x, state:%x\r\n", SLE_UART_SERVER_LOG, announce_id,
        status);
}

static void sle_announce_terminal_cbk(uint32_t announce_id)
{
    sample_at_log_print("%s sle announce terminal callback id:%02x\r\n", SLE_UART_SERVER_LOG, announce_id);
}

errcode_t sle_uart_announce_seek_merge_cbks(const sle_announce_seek_callbacks_t *cbks)
{
    sle_announce_seek_callbacks_t merged_cbks;
    errcode_t ret;

    if (cbks == NULL) {
        return ERRCODE_SLE_FAIL;
    }
    /* Merge preserves server callbacks while letting the client inject seek hooks. */
    merged_cbks = g_sle_uart_announce_seek_cbks;
    if (cbks->sle_enable_cb != NULL) {
        merged_cbks.sle_enable_cb = cbks->sle_enable_cb;
    }
    if (cbks->announce_enable_cb != NULL) {
        merged_cbks.announce_enable_cb = cbks->announce_enable_cb;
    }
    if (cbks->announce_disable_cb != NULL) {
        merged_cbks.announce_disable_cb = cbks->announce_disable_cb;
    }
    if (cbks->announce_terminal_cb != NULL) {
        merged_cbks.announce_terminal_cb = cbks->announce_terminal_cb;
    }
    if (cbks->seek_enable_cb != NULL) {
        merged_cbks.seek_enable_cb = cbks->seek_enable_cb;
    }
    if (cbks->seek_disable_cb != NULL) {
        merged_cbks.seek_disable_cb = cbks->seek_disable_cb;
    }
    if (cbks->seek_result_cb != NULL) {
        merged_cbks.seek_result_cb = cbks->seek_result_cb;
    }
    ret = sle_announce_seek_register_callbacks(&merged_cbks);
    if (ret == ERRCODE_SLE_SUCCESS) {
        g_sle_uart_announce_seek_cbks = merged_cbks;
    }
    return ret;
}

errcode_t sle_uart_announce_register_cbks(void)
{
    errcode_t ret = 0;
    sle_announce_seek_callbacks_t seek_cbks = {0};
    seek_cbks.announce_enable_cb = sle_announce_enable_cbk;
    seek_cbks.announce_disable_cb = sle_announce_disable_cbk;
    seek_cbks.announce_terminal_cb = sle_announce_terminal_cbk;
    ret = sle_uart_announce_seek_merge_cbks(&seek_cbks);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_announce_register_cbks,register_callbacks fail :%x\r\n",
            SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}

errcode_t sle_uart_server_adv_init(void)
{
    errcode_t ret;

    /* Start advertising only after the payload, name, and route metadata are ready. */
    sle_set_default_announce_param();
    sle_set_default_announce_data();
    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_adv_init,sle_start_announce fail :%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}

errcode_t sle_uart_server_adv_restart(void)
{
    errcode_t ret;

    /* Simple restart is enough because the payload is rebuilt from current globals. */
    (void)sle_stop_announce(SLE_ADV_HANDLE_DEFAULT);
    ret = sle_start_announce(SLE_ADV_HANDLE_DEFAULT);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_adv_restart fail:%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    sample_at_log_print("%s sle_uart_server_adv_restart ok\r\n", SLE_UART_SERVER_LOG);
    return ERRCODE_SLE_SUCCESS;
}
