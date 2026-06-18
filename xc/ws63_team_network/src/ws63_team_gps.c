#include "ws63_team_gps.h"

#include "common_def.h"
#include "errcode.h"
#include "pinctrl.h"
#include "securec.h"
#include "soc_osal.h"
#include "sle_team_nmea.h"
#include "uart.h"

#ifndef CONFIG_SLE_TEAM_GPS_ENABLE
#define CONFIG_SLE_TEAM_GPS_ENABLE 0
#endif
#ifndef CONFIG_SLE_TEAM_GPS_UART_BUS
#define CONFIG_SLE_TEAM_GPS_UART_BUS 1
#endif
#ifndef CONFIG_SLE_TEAM_GPS_UART_TXD_PIN
#define CONFIG_SLE_TEAM_GPS_UART_TXD_PIN 17
#endif
#ifndef CONFIG_SLE_TEAM_GPS_UART_RXD_PIN
#define CONFIG_SLE_TEAM_GPS_UART_RXD_PIN 18
#endif
#ifndef CONFIG_SLE_TEAM_GPS_BAUDRATE
#define CONFIG_SLE_TEAM_GPS_BAUDRATE 9600
#endif

/*
 * GPS helper for the board app.
 *
 * The UART ISR feeds NMEA bytes into the parser, the parser caches the latest
 * valid fix, and the networking task periodically publishes that fix to the
 * leader only when the local node is a joined member.
 */
#define WS63_TEAM_GPS_RX_BUF_SIZE 256U
#define WS63_TEAM_GPS_LINE_SIZE 96U

typedef struct {
    uint8_t rx_buf[WS63_TEAM_GPS_RX_BUF_SIZE];
    char line_buf[WS63_TEAM_GPS_LINE_SIZE];
    size_t line_len;
    sle_team_nmea_state_t nmea;
    sle_team_pos_body_t last_pos;
    uint8_t ready;
    uint8_t has_fix;
    uint32_t last_report_ms;
} ws63_team_gps_state_t;

static ws63_team_gps_state_t g_gps;
static uart_buffer_config_t g_gps_uart_buffer = {
    .rx_buffer = g_gps.rx_buf,
    .rx_buffer_size = WS63_TEAM_GPS_RX_BUF_SIZE,
};

static void gps_rx_cb(const void *buffer, uint16_t length, bool error)
{
    const uint8_t *data = (const uint8_t *)buffer;
    uint16_t i;

    if (error || data == NULL) {
        return;
    }
    /* UART callback receives arbitrary byte chunks, not complete NMEA lines.
     * Feed bytes one at a time; the parser returns SLE_TEAM_OK only when a
     * complete supported sentence has produced a position body. */
    for (i = 0U; i < length; i++) {
        sle_team_pos_body_t pos = {0};
        int ret = sle_team_nmea_feed(&g_gps.nmea, (char)data[i], g_gps.line_buf,
            sizeof(g_gps.line_buf), &g_gps.line_len, &pos);

        if (ret == SLE_TEAM_OK && pos.fix_status != 0U) {
            g_gps.last_pos = pos;
            g_gps.has_fix = 1U;
        }
    }
}

void ws63_team_gps_init(void)
{
#if defined(CONFIG_SLE_TEAM_GPS_ENABLE) && CONFIG_SLE_TEAM_GPS_ENABLE
    uart_attr_t attr = {
        .baud_rate = CONFIG_SLE_TEAM_GPS_BAUDRATE,
        .data_bits = UART_DATA_BIT_8,
        .stop_bits = UART_STOP_BIT_1,
        .parity = UART_PARITY_NONE,
    };
    uart_pin_config_t pin_config = {
        .tx_pin = CONFIG_SLE_TEAM_GPS_UART_TXD_PIN,
        .rx_pin = CONFIG_SLE_TEAM_GPS_UART_RXD_PIN,
        .cts_pin = PIN_NONE,
        .rts_pin = PIN_NONE,
    };
    errcode_t ret;

    /* GPS is optional hardware. When enabled, this task only initializes UART
     * and caches the latest fix; network transmission happens from tick(). */
    (void)memset_s(&g_gps, sizeof(g_gps), 0, sizeof(g_gps));
    sle_team_nmea_init(&g_gps.nmea);
    (void)uapi_pin_set_mode(CONFIG_SLE_TEAM_GPS_UART_TXD_PIN, PIN_MODE_1);
    (void)uapi_pin_set_mode(CONFIG_SLE_TEAM_GPS_UART_RXD_PIN, PIN_MODE_1);
    (void)uapi_uart_deinit(CONFIG_SLE_TEAM_GPS_UART_BUS);
    ret = uapi_uart_init(CONFIG_SLE_TEAM_GPS_UART_BUS, &pin_config, &attr, NULL, &g_gps_uart_buffer);
    if (ret == ERRCODE_SUCC) {
        ret = uapi_uart_register_rx_callback(CONFIG_SLE_TEAM_GPS_UART_BUS,
            UART_RX_CONDITION_FULL_OR_IDLE, 1, gps_rx_cb);
    }
    g_gps.ready = ret == ERRCODE_SUCC ? 1U : 0U;
    osal_printk("[team-gps] init enabled bus=%u baud=%u ret=0x%x\r\n",
        CONFIG_SLE_TEAM_GPS_UART_BUS, CONFIG_SLE_TEAM_GPS_BAUDRATE, ret);
#else
    (void)memset_s(&g_gps, sizeof(g_gps), 0, sizeof(g_gps));
    sle_team_nmea_init(&g_gps.nmea);
    osal_printk("[team-gps] disabled\r\n");
#endif
}

void ws63_team_gps_tick(sle_team_node_t *node, uint32_t now_ms, uint8_t battery_percent)
{
    /* Members periodically report their latest valid GPS fix to the leader.
     * Leaders do not send GPS upstream, and no packet is sent before a fix. */
    if (node == NULL || g_gps.ready == 0U || g_gps.has_fix == 0U ||
        node->cfg.role != SLE_TEAM_ROLE_MEMBER || node->joined == 0U ||
        node->cfg.report_interval_s == 0U) {
        return;
    }
    if (g_gps.last_report_ms != 0U &&
        (uint32_t)(now_ms - g_gps.last_report_ms) < (uint32_t)node->cfg.report_interval_s * 1000U) {
        return;
    }
    g_gps.last_report_ms = now_ms;
    g_gps.last_pos.battery_percent = battery_percent;
    (void)sle_team_node_send_position(node, node->cfg.leader_id, &g_gps.last_pos);
}
