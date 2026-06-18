/**
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2023-2023. All rights reserved.
 *
 * Description: SLE UART sample of client. \n
 *
 * History: \n
 * 2023-04-03, Create file. \n
 */
#ifndef SLE_UART_CLIENT_H
#define SLE_UART_CLIENT_H

#include "sle_connection_manager.h"
#include "sle_device_discovery.h"
#include "sle_ssap_client.h"

typedef uint8_t (*sle_uart_client_seek_filter_cb)(const sle_seek_result_info_t *seek_result_data, void *user_ctx);

/* Client role bootstrap: enable SLE, register callbacks, then start scanning. */
void sle_uart_client_init(ssapc_notification_callback notification_cb, ssapc_indication_callback indication_cb);

/* App-level filter applied before a seek result consumes a connection slot. */
void sle_uart_client_set_seek_filter(sle_uart_client_seek_filter_cb seek_filter, void *user_ctx);
void sle_uart_client_set_connect_limit(uint8_t max_conns);

/* Connection-table helpers for mesh routing and packet-binding recovery. */
uint8_t sle_uart_client_has_conn(uint16_t conn_id);
uint8_t sle_uart_client_is_pending_remote_addr(const sle_addr_t *addr);
uint8_t sle_uart_client_get_active_conns(uint16_t *conn_ids, uint8_t max_conns);
uint8_t sle_uart_client_get_conn_member(uint16_t conn_id, uint8_t *member_id);
uint8_t sle_uart_client_disconnect_conn(uint16_t conn_id);
void sle_uart_client_handle_connect_state_changed(uint16_t conn_id, const sle_addr_t *addr,
    sle_acb_state_t conn_state, sle_pair_state_t pair_state, sle_disc_reason_t disc_reason);
void sle_uart_client_handle_pair_complete(uint16_t conn_id, const sle_addr_t *addr, errcode_t status);
void sle_uart_client_handle_read_rssi(uint16_t conn_id, int8_t rssi, errcode_t status);

/* Scan/connection pacing is tick-driven so the app task stays in control. */
void sle_uart_start_scan(void);

uint16_t get_g_sle_uart_conn_id(void);
int8_t sle_uart_client_get_last_rssi(void);
errcode_t sle_uart_client_read_remote_rssi(void);
uint8_t sle_uart_client_is_ready(void);
uint16_t sle_uart_client_connected_count(void);
uint8_t sle_uart_client_scan_busy(void);
void sle_uart_client_force_rescan(void);
void sle_uart_client_pause_scan_request(const char *reason);
void sle_uart_client_pause_scan(const char *reason);
void sle_uart_client_resume_scan(const char *reason);

/* Send helpers push packets through one bound SSAP link or all active links. */
errcode_t sle_uart_client_send_by_conn(uint16_t conn_id, const uint8_t *data, uint16_t len);
errcode_t sle_uart_client_send_all(const uint8_t *data, uint16_t len);
uint8_t sle_uart_client_bind_member_conn(uint8_t member_id, uint16_t conn_id);
uint8_t sle_uart_client_find_conn_by_member(uint8_t member_id, uint16_t *conn_id);
void sle_uart_client_tick(void);

/* SSAP callback glue used by the WS63 app. */
ssapc_write_param_t *get_g_sle_uart_send_param(void);
void sle_uart_notification_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);
void sle_uart_indication_cb(uint8_t client_id, uint16_t conn_id, ssapc_handle_value_t *data, errcode_t status);

#endif
