/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE UART Server Source. \n
 *
 * History: \n
 * 2023-07-17, Create file. \n
 */
#include "common_def.h"
#include "securec.h"
#include "soc_osal.h"
#include "sle_errcode.h"
#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_team_packet.h"
#include "sle_uart_server_adv.h"
#include "sle_uart_server.h"
#define OCTET_BIT_LEN           8
#define UUID_LEN_2              2
#define UUID_INDEX              14
#define BT_INDEX_4              4
#define BT_INDEX_0              0
#define UART_BUFF_LENGTH        0x100
#define SLE_UART_SERVER_MAX_CONNECTIONS 8
#define SLE_UART_MEMBER_ID_MAX 254U
#define SLE_UART_BROADCAST_ID 0xFFU

/* 广播ID */
#define SLE_ADV_HANDLE_DEFAULT  1
/* sle server app uuid for test */
static char g_sle_uuid_app_uuid[UUID_LEN_2] = { 0x12, 0x34 };
/* server notify property uuid for test */
static char g_sle_property_value[OCTET_BIT_LEN] = { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 };
typedef struct {
    uint8_t active;
    uint16_t conn_id;
    uint8_t member_id;
    int8_t rssi;
    sle_addr_t addr;
} sle_uart_server_conn_t;

static sle_uart_server_conn_t g_sle_conns[SLE_UART_SERVER_MAX_CONNECTIONS];
static uint8_t g_sle_conn_count = 0;
/* sle server handle */
static uint8_t g_server_id = 0;
/* sle service handle */
static uint16_t g_service_handle = 0;
/* sle ntf property handle */
static uint16_t g_property_handle = 0;
/* sle pair acb handle */
uint16_t g_sle_pair_hdl;

#define UUID_16BIT_LEN 2
#define UUID_128BIT_LEN 16
#define sample_at_log_print(fmt, args...) osal_printk(fmt, ##args)
#define SLE_UART_SERVER_LOG "[sle uart server]"
#define SLE_SERVER_INIT_DELAY_MS    1000
static sle_uart_server_msg_queue g_sle_uart_server_msg_queue = NULL;
static uint8_t g_sle_uart_base[] = { 0x37, 0xBE, 0xA8, 0x80, 0xFC, 0x70, 0x11, 0xEA, \
    0xB7, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 };

uint16_t get_connect_id(void)
{
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active != 0U) {
            return g_sle_conns[i].conn_id;
        }
    }
    return 0;
}

int8_t sle_uart_server_get_last_rssi(void)
{
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active != 0U) {
            return g_sle_conns[i].rssi;
        }
    }
    return SLE_TEAM_RSSI_UNKNOWN;
}

errcode_t sle_uart_server_read_remote_rssi(void)
{
    uint8_t requested = 0U;

    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active == 0U) {
            continue;
        }
        errcode_t ret = sle_read_remote_device_rssi(g_sle_conns[i].conn_id);
        if (ret == ERRCODE_SLE_SUCCESS) {
            requested = 1U;
        }
    }
    if (requested == 0U) {
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static sle_uart_server_conn_t *sle_uart_server_find_conn(uint16_t conn_id)
{
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active != 0U && g_sle_conns[i].conn_id == conn_id) {
            return &g_sle_conns[i];
        }
    }
    return NULL;
}

uint8_t sle_uart_server_has_conn(uint16_t conn_id)
{
    return sle_uart_server_find_conn(conn_id) != NULL ? 1U : 0U;
}

static sle_uart_server_conn_t *sle_uart_server_alloc_conn(uint16_t conn_id)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);
    if (conn != NULL) {
        return conn;
    }
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active == 0U) {
            g_sle_conns[i].active = 1U;
            g_sle_conns[i].conn_id = conn_id;
            g_sle_conns[i].member_id = 0U;
            g_sle_conns[i].rssi = SLE_TEAM_RSSI_UNKNOWN;
            g_sle_conn_count++;
            return &g_sle_conns[i];
        }
    }
    return NULL;
}

static sle_uart_server_conn_t *sle_uart_server_find_conn_slot_by_member(uint8_t member_id)
{
    if (member_id == 0U || member_id == SLE_UART_BROADCAST_ID) {
        return NULL;
    }
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active != 0U && g_sle_conns[i].member_id == member_id) {
            return &g_sle_conns[i];
        }
    }
    return NULL;
}

uint16_t sle_uart_server_find_conn_by_member(uint8_t member_id)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn_slot_by_member(member_id);
    return conn == NULL ? 0U : conn->conn_id;
}

uint8_t sle_uart_server_find_conn_by_member_ex(uint8_t member_id, uint16_t *conn_id)
{
    sle_uart_server_conn_t *conn;

    if (conn_id == NULL) {
        return 0U;
    }
    conn = sle_uart_server_find_conn_slot_by_member(member_id);
    if (conn == NULL) {
        return 0U;
    }
    *conn_id = conn->conn_id;
    return 1U;
}

uint8_t sle_uart_server_bind_member_conn(uint8_t member_id, uint16_t conn_id)
{
    sle_uart_server_conn_t *conn;

    if (member_id == 0U || member_id > SLE_UART_MEMBER_ID_MAX) {
        return 0U;
    }
    conn = sle_uart_server_find_conn(conn_id);
    if (conn == NULL) {
        conn = sle_uart_server_alloc_conn(conn_id);
        if (conn == NULL) {
            sample_at_log_print("%s bind member:%u conn_id:%x failed: table full\r\n",
                SLE_UART_SERVER_LOG, member_id, conn_id);
            return 0U;
        }
        sample_at_log_print("%s recover conn_id:%x from packet bind member:%u\r\n",
            SLE_UART_SERVER_LOG, conn_id, member_id);
    }
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active != 0U && g_sle_conns[i].conn_id != conn_id &&
            g_sle_conns[i].member_id == member_id) {
            g_sle_conns[i].member_id = 0U;
        }
    }
    conn->member_id = member_id;
    sample_at_log_print("%s bind member:%u conn_id:%x\r\n", SLE_UART_SERVER_LOG, member_id, conn_id);
    return 1U;
}

static void sle_uart_server_remove_conn(uint16_t conn_id)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);
    if (conn == NULL) {
        return;
    }
    (void)memset_s(conn, sizeof(*conn), 0, sizeof(*conn));
    conn->rssi = SLE_TEAM_RSSI_UNKNOWN;
    if (g_sle_conn_count > 0U) {
        g_sle_conn_count--;
    }
}

static void encode2byte_little(uint8_t *_ptr, uint16_t data)
{
    *(uint8_t *)((_ptr) + 1) = (uint8_t)((data) >> 0x8);
    *(uint8_t *)(_ptr) = (uint8_t)(data);
}

static void sle_uuid_set_base(sle_uuid_t *out)
{
    errcode_t ret;
    ret = memcpy_s(out->uuid, SLE_UUID_LEN, g_sle_uart_base, SLE_UUID_LEN);
    if (ret != EOK) {
        sample_at_log_print("%s sle_uuid_set_base memcpy fail\n", SLE_UART_SERVER_LOG);
        out->len = 0;
        return ;
    }
    out->len = UUID_LEN_2;
}

static void sle_uuid_setu2(uint16_t u2, sle_uuid_t *out)
{
    sle_uuid_set_base(out);
    out->len = UUID_LEN_2;
    encode2byte_little(&out->uuid[UUID_INDEX], u2);
}
static void sle_uart_uuid_print(sle_uuid_t *uuid)
{
    if (uuid == NULL) {
        sample_at_log_print("%s uuid_print,uuid is null\r\n", SLE_UART_SERVER_LOG);
        return;
    }
    if (uuid->len == UUID_16BIT_LEN) {
        sample_at_log_print("%s uuid: %02x %02x.\n", SLE_UART_SERVER_LOG,
            uuid->uuid[14], uuid->uuid[15]); /* 14 15: uuid index */
    } else if (uuid->len == UUID_128BIT_LEN) {
        sample_at_log_print("%s uuid: \n", SLE_UART_SERVER_LOG); /* 14 15: uuid index */
        sample_at_log_print("%s 0x%02x 0x%02x 0x%02x \n", SLE_UART_SERVER_LOG, uuid->uuid[0], uuid->uuid[1],
            uuid->uuid[2], uuid->uuid[3]);
        sample_at_log_print("%s 0x%02x 0x%02x 0x%02x \n", SLE_UART_SERVER_LOG, uuid->uuid[4], uuid->uuid[5],
            uuid->uuid[6], uuid->uuid[7]);
        sample_at_log_print("%s 0x%02x 0x%02x 0x%02x \n", SLE_UART_SERVER_LOG, uuid->uuid[8], uuid->uuid[9],
            uuid->uuid[10], uuid->uuid[11]);
        sample_at_log_print("%s 0x%02x 0x%02x 0x%02x \n", SLE_UART_SERVER_LOG, uuid->uuid[12], uuid->uuid[13],
            uuid->uuid[14], uuid->uuid[15]);
    }
}

static void ssaps_mtu_changed_cbk(uint8_t server_id, uint16_t conn_id,  ssap_exchange_info_t *mtu_size,
    errcode_t status)
{
    sample_at_log_print("%s ssaps ssaps_mtu_changed_cbk callback server_id:%x, conn_id:%x, mtu_size:%x, status:%x\r\n",
        SLE_UART_SERVER_LOG, server_id, conn_id, mtu_size->mtu_size, status);
    if (g_sle_pair_hdl == 0) {
        g_sle_pair_hdl = conn_id + 1;
    }
}

static void ssaps_start_service_cbk(uint8_t server_id, uint16_t handle, errcode_t status)
{
    sample_at_log_print("%s start service cbk callback server_id:%d, handle:%x, status:%x\r\n", SLE_UART_SERVER_LOG,
        server_id, handle, status);
}
static void ssaps_add_service_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t handle, errcode_t status)
{
    sample_at_log_print("%s add service cbk callback server_id:%x, handle:%x, status:%x\r\n", SLE_UART_SERVER_LOG,
        server_id, handle, status);
    sle_uart_uuid_print(uuid);
}
static void ssaps_add_property_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t service_handle,
    uint16_t handle, errcode_t status)
{
    sample_at_log_print("%s add property cbk callback server_id:%x, service_handle:%x,handle:%x, status:%x\r\n",
        SLE_UART_SERVER_LOG, server_id, service_handle, handle, status);
    sle_uart_uuid_print(uuid);
}
static void ssaps_add_descriptor_cbk(uint8_t server_id, sle_uuid_t *uuid, uint16_t service_handle,
    uint16_t property_handle, errcode_t status)
{
    sample_at_log_print("%s add descriptor cbk callback server_id:%x, service_handle:%x, property_handle:%x, \
        status:%x\r\n", SLE_UART_SERVER_LOG, server_id, service_handle, property_handle, status);
    sle_uart_uuid_print(uuid);
}
static void ssaps_delete_all_service_cbk(uint8_t server_id, errcode_t status)
{
    sample_at_log_print("%s delete all service callback server_id:%x, status:%x\r\n", SLE_UART_SERVER_LOG,
        server_id, status);
}
static errcode_t sle_ssaps_register_cbks(ssaps_read_request_callback ssaps_read_callback, ssaps_write_request_callback
    ssaps_write_callback)
{
    errcode_t ret;
    ssaps_callbacks_t ssaps_cbk = {0};
    ssaps_cbk.add_service_cb = ssaps_add_service_cbk;
    ssaps_cbk.add_property_cb = ssaps_add_property_cbk;
    ssaps_cbk.add_descriptor_cb = ssaps_add_descriptor_cbk;
    ssaps_cbk.start_service_cb = ssaps_start_service_cbk;
    ssaps_cbk.delete_all_service_cb = ssaps_delete_all_service_cbk;
    ssaps_cbk.mtu_changed_cb = ssaps_mtu_changed_cbk;
    ssaps_cbk.read_request_cb = ssaps_read_callback;
    ssaps_cbk.write_request_cb = ssaps_write_callback;
    ret = ssaps_register_callbacks(&ssaps_cbk);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_ssaps_register_cbks,ssaps_register_callbacks fail :%x\r\n", SLE_UART_SERVER_LOG,
            ret);
        return ret;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_service_add(void)
{
    errcode_t ret;
    sle_uuid_t service_uuid = {0};
    sle_uuid_setu2(SLE_UUID_SERVER_SERVICE, &service_uuid);
    ret = ssaps_add_service_sync(g_server_id, &service_uuid, 1, &g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle uuid add service fail, ret:%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ERRCODE_SLE_FAIL;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uuid_server_property_add(void)
{
    errcode_t ret;
    ssaps_property_info_t property = {0};
    ssaps_desc_info_t descriptor = {0};
    uint8_t ntf_value[] = {0x01, 0x0};

    property.permissions = SLE_UUID_TEST_PROPERTIES;
    property.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_NOTIFY;
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &property.uuid);
    property.value = (uint8_t *)osal_vmalloc(sizeof(g_sle_property_value));
    if (property.value == NULL) {
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(property.value, sizeof(g_sle_property_value), g_sle_property_value,
        sizeof(g_sle_property_value)) != EOK) {
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    ret = ssaps_add_property_sync(g_server_id, g_service_handle, &property,  &g_property_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle uart add property fail, ret:%x\r\n", SLE_UART_SERVER_LOG, ret);
        osal_vfree(property.value);
        return ERRCODE_SLE_FAIL;
    }
    descriptor.permissions = SLE_UUID_TEST_DESCRIPTOR;
    descriptor.type = SSAP_DESCRIPTOR_USER_DESCRIPTION;
    descriptor.operate_indication = SSAP_OPERATE_INDICATION_BIT_READ | SSAP_OPERATE_INDICATION_BIT_WRITE;
    descriptor.value = ntf_value;
    descriptor.value_len = sizeof(ntf_value);

    ret = ssaps_add_descriptor_sync(g_server_id, g_service_handle, g_property_handle, &descriptor);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle uart add descriptor fail, ret:%x\r\n", SLE_UART_SERVER_LOG, ret);
        osal_vfree(property.value);
        osal_vfree(descriptor.value);
        return ERRCODE_SLE_FAIL;
    }
    osal_vfree(property.value);
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uart_server_add(void)
{
    errcode_t ret;
    sle_uuid_t app_uuid = {0};

    sample_at_log_print("%s sle uart add service in\r\n", SLE_UART_SERVER_LOG);
    app_uuid.len = sizeof(g_sle_uuid_app_uuid);
    if (memcpy_s(app_uuid.uuid, app_uuid.len, g_sle_uuid_app_uuid, sizeof(g_sle_uuid_app_uuid)) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    ssaps_register_server(&app_uuid, &g_server_id);

    if (sle_uuid_server_service_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    if (sle_uuid_server_property_add() != ERRCODE_SLE_SUCCESS) {
        ssaps_unregister_server(g_server_id);
        return ERRCODE_SLE_FAIL;
    }
    sample_at_log_print("%s sle uart add service, server_id:%x, service_handle:%x, property_handle:%x\r\n",
        SLE_UART_SERVER_LOG, g_server_id, g_service_handle, g_property_handle);
    ret = ssaps_start_service(g_server_id, g_service_handle);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle uart add service fail, ret:%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ERRCODE_SLE_FAIL;
    }
    sample_at_log_print("%s sle uart add service out\r\n", SLE_UART_SERVER_LOG);
    return ERRCODE_SLE_SUCCESS;
}

/* device通过uuid向host发送数据：report */
errcode_t sle_uart_server_send_report_by_uuid(const uint8_t *data, uint8_t len)
{
    errcode_t first_fail = ERRCODE_SLE_FAIL;
    uint8_t failed = 0U;
    uint8_t sent = 0U;
    ssaps_ntf_ind_by_uuid_t param = {0};
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.start_handle = g_service_handle;
    param.end_handle = g_property_handle;
    param.value_len = len;
    param.value = (uint8_t *)osal_vmalloc(len);
    if (param.value == NULL) {
        sample_at_log_print("%s send report new fail\r\n", SLE_UART_SERVER_LOG);
        return ERRCODE_SLE_FAIL;
    }
    if (memcpy_s(param.value, param.value_len, data, len) != EOK) {
        sample_at_log_print("%s send input report memcpy fail\r\n", SLE_UART_SERVER_LOG);
        osal_vfree(param.value);
        return ERRCODE_SLE_FAIL;
    }
    sle_uuid_setu2(SLE_UUID_SERVER_NTF_REPORT, &param.uuid);
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active == 0U) {
            continue;
        }
        errcode_t ret = ssaps_notify_indicate_by_uuid(g_server_id, g_sle_conns[i].conn_id, &param);
        if (ret == ERRCODE_SLE_SUCCESS) {
            sent = 1U;
        } else if (failed == 0U) {
            first_fail = ret;
            failed = 1U;
        }
    }
    osal_vfree(param.value);
    if (sent == 0U) {
        sample_at_log_print("%s send_report_by_uuid no successful links fail:%x\r\n",
            SLE_UART_SERVER_LOG, first_fail);
        return first_fail;
    }
    return ERRCODE_SLE_SUCCESS;
}

static errcode_t sle_uart_server_send_report_to_conn(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    ssaps_ntf_ind_t param = {0};
    uint8_t receive_buf[UART_BUFF_LENGTH] = { 0 }; /* max receive length. */

    if (data == NULL || len == 0U || len > sizeof(receive_buf)) {
        return ERRCODE_SLE_FAIL;
    }
    param.handle = g_property_handle;
    param.type = SSAP_PROPERTY_TYPE_VALUE;
    param.value = receive_buf;
    param.value_len = len;
    if (memcpy_s(param.value, sizeof(receive_buf), data, len) != EOK) {
        return ERRCODE_SLE_FAIL;
    }
    return ssaps_notify_indicate(g_server_id, conn_id, &param);
}

errcode_t sle_uart_server_send_report_by_conn(uint16_t conn_id, const uint8_t *data, uint16_t len)
{
    if (sle_uart_server_find_conn(conn_id) == NULL) {
        return ERRCODE_SLE_FAIL;
    }
    return sle_uart_server_send_report_to_conn(conn_id, data, len);
}

/* device通过handle向host发送数据：report */
errcode_t sle_uart_server_send_report_by_handle(const uint8_t *data, uint16_t len)
{
    errcode_t first_fail = ERRCODE_SLE_FAIL;
    uint8_t failed = 0U;
    uint8_t sent = 0U;

    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active == 0U) {
            continue;
        }
        errcode_t ret = sle_uart_server_send_report_to_conn(g_sle_conns[i].conn_id, data, len);
        if (ret == ERRCODE_SLE_SUCCESS) {
            sent = 1U;
        } else if (failed == 0U) {
            first_fail = ret;
            failed = 1U;
        }
    }
    if (sent == 0U) {
        return first_fail;
    }
    return ERRCODE_SLE_SUCCESS;
}

void sle_uart_server_handle_connect_state_changed(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason)
{
    uint8_t sle_connect_state[] = "sle_dis_connect";
    sample_at_log_print("%s connect state changed callback conn_id:0x%02x, conn_state:0x%x, pair_state:0x%x, \
        disc_reason:0x%x\r\n", SLE_UART_SERVER_LOG,conn_id, conn_state, pair_state, disc_reason);
    if (addr != NULL) {
        sample_at_log_print("%s connect state changed callback addr:%02x:**:**:**:%02x:%02x\r\n",
            SLE_UART_SERVER_LOG, addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4]);
    }
    if (conn_state == SLE_ACB_STATE_CONNECTED) {
        sle_uart_server_conn_t *conn = sle_uart_server_alloc_conn(conn_id);
        if (conn == NULL) {
            sample_at_log_print("%s connection table full conn_id:%x\r\n", SLE_UART_SERVER_LOG, conn_id);
            if (addr != NULL) {
                (void)sle_disconnect_remote_device(addr);
            }
        } else if (addr != NULL) {
            (void)memcpy_s(&conn->addr, sizeof(conn->addr), addr, sizeof(*addr));
        }
        g_sle_pair_hdl = (g_sle_conn_count > 0U) ? (get_connect_id() + 1U) : 0U;
        (void)sle_read_remote_device_rssi(conn_id);
        sample_at_log_print("%s keep announce stable after connect\r\n", SLE_UART_SERVER_LOG);
        sample_at_log_print("%s connected count:%u\r\n", SLE_UART_SERVER_LOG, g_sle_conn_count);
    } else if (conn_state == SLE_ACB_STATE_DISCONNECTED) {
        sle_uart_server_remove_conn(conn_id);
        g_sle_pair_hdl = (g_sle_conn_count > 0U) ? (get_connect_id() + 1U) : 0U;
        (void)sle_uart_server_adv_restart();
        sample_at_log_print("%s disconnected count:%u\r\n", SLE_UART_SERVER_LOG, g_sle_conn_count);
        if (g_sle_uart_server_msg_queue != NULL) {
            g_sle_uart_server_msg_queue(sle_connect_state, sizeof(sle_connect_state));
        }
    }
}

void sle_uart_server_handle_read_rssi(uint16_t conn_id, int8_t rssi, errcode_t status)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);

    if (status == ERRCODE_SLE_SUCCESS && conn != NULL) {
        conn->rssi = rssi;
        sample_at_log_print("%s rssi conn_id:%d rssi:%d\r\n", SLE_UART_SERVER_LOG, conn_id, rssi);
    }
}

void sle_uart_server_handle_pair_complete(uint16_t conn_id, const sle_addr_t *addr, errcode_t status)
{
    sample_at_log_print("%s pair complete conn_id:%02x, status:%x\r\n", SLE_UART_SERVER_LOG,
        conn_id, status);
    if (addr != NULL) {
        sample_at_log_print("%s pair complete addr:%02x:**:**:**:%02x:%02x\r\n",
            SLE_UART_SERVER_LOG, addr->addr[BT_INDEX_0], addr->addr[BT_INDEX_4]);
    }
    if (sle_uart_server_alloc_conn(conn_id) == NULL && addr != NULL) {
        (void)sle_disconnect_remote_device(addr);
    }
    g_sle_pair_hdl = (g_sle_conn_count > 0U) ? (get_connect_id() + 1U) : (conn_id + 1U);
    ssap_exchange_info_t parameter = { 0 };
    errcode_t ret;
    parameter.mtu_size = 520;
    parameter.version = 1;
    ret = ssaps_set_info(g_server_id, &parameter);
    sample_at_log_print("%s ssaps_set_info ret:%x mtu:%u version:%u\r\n",
        SLE_UART_SERVER_LOG, ret, parameter.mtu_size, parameter.version);
}

uint16_t sle_uart_server_connected_count(void)
{
    return g_sle_conn_count;
}

uint16_t sle_uart_client_is_connected(void)
{
    return sle_uart_server_connected_count();
}

uint8_t sle_uart_server_get_active_conns(uint16_t *conn_ids, uint8_t max_conns)
{
    uint8_t count = 0U;

    if (conn_ids == NULL || max_conns == 0U) {
        return 0U;
    }
    for (uint8_t i = 0U; i < SLE_UART_SERVER_MAX_CONNECTIONS && count < max_conns; i++) {
        if (g_sle_conns[i].active == 0U) {
            continue;
        }
        conn_ids[count++] = g_sle_conns[i].conn_id;
    }
    return count;
}

uint8_t sle_uart_server_get_conn_member(uint16_t conn_id, uint8_t *member_id)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);

    if (conn == NULL || member_id == NULL) {
        return 0U;
    }
    *member_id = conn->member_id;
    return 1U;
}

uint8_t sle_uart_server_get_conn_rssi(uint16_t conn_id, int8_t *rssi)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);

    if (conn == NULL || rssi == NULL) {
        return 0U;
    }
    *rssi = conn->rssi;
    return 1U;
}

errcode_t sle_uart_server_disconnect_conn(uint16_t conn_id)
{
    sle_uart_server_conn_t *conn = sle_uart_server_find_conn(conn_id);

    if (conn == NULL) {
        return ERRCODE_SLE_FAIL;
    }
    return sle_disconnect_remote_device(&conn->addr);
}

errcode_t sle_uart_server_disconnect_current(void)
{
    if (g_sle_conn_count == 0U) {
        return ERRCODE_SUCC;
    }
    for (uint8_t i = 0; i < SLE_UART_SERVER_MAX_CONNECTIONS; i++) {
        if (g_sle_conns[i].active == 0U) {
            continue;
        }
        errcode_t ret = sle_disconnect_remote_device(&g_sle_conns[i].addr);
        sample_at_log_print("%s disconnect conn_id:%x ret:%x\r\n",
            SLE_UART_SERVER_LOG, g_sle_conns[i].conn_id, ret);
    }
    return ERRCODE_SUCC;
}

/* 初始化uuid server */
errcode_t sle_uart_server_init(ssaps_read_request_callback ssaps_read_callback, ssaps_write_request_callback
    ssaps_write_callback)
{
    errcode_t ret;

    /* 使能SLE */
    if (enable_sle() != ERRCODE_SUCC) {
        sample_at_log_print("[SLE Server] sle enbale fail !\r\n");
        return -1;
    }

    ret = sle_uart_announce_register_cbks();
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_init,sle_uart_announce_register_cbks fail :%x\r\n",
        SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    ret = sle_ssaps_register_cbks(ssaps_read_callback, ssaps_write_callback);
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_init,sle_ssaps_register_cbks fail :%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    ret = sle_uart_server_add();
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_init,sle_uart_server_add fail :%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    ret = sle_uart_server_adv_init();
    if (ret != ERRCODE_SLE_SUCCESS) {
        sample_at_log_print("%s sle_uart_server_init,sle_uart_server_adv_init fail :%x\r\n", SLE_UART_SERVER_LOG, ret);
        return ret;
    }
    sample_at_log_print("%s init ok\r\n", SLE_UART_SERVER_LOG);
    return ERRCODE_SLE_SUCCESS;
}


void sle_uart_server_register_msg(sle_uart_server_msg_queue sle_uart_server_msg)
{
    g_sle_uart_server_msg_queue = sle_uart_server_msg;
}
