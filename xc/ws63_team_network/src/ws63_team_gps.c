#include "ws63_team_gps.h"

#include "common_def.h"
#include "errcode.h"
#include "pinctrl.h"
#include "securec.h"
#include "soc_osal.h"
#include "sle_team_nmea.h"
#include "tcxo.h"
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
 * The UART ISR feeds NMEA bytes into the parser, and the main task keeps a
 * board-local GPS cache even before the node joins a mesh. Upstream POS_REPORT
 * transmission stays gated by member join state.
 */
#define WS63_TEAM_GPS_RX_BUF_SIZE 256U
#define WS63_TEAM_GPS_LINE_SIZE 96U
#define WS63_TEAM_GPS_SOURCE_NONE 0U
#define WS63_TEAM_GPS_SOURCE_NMEA 1U
#define WS63_TEAM_GPS_SOURCE_FALLBACK 2U

typedef struct {
    uint8_t rx_buf[WS63_TEAM_GPS_RX_BUF_SIZE];
    char line_buf[WS63_TEAM_GPS_LINE_SIZE];
    size_t line_len;
    sle_team_nmea_state_t nmea;
    sle_team_pos_body_t last_pos;
    uint8_t ready;
    uint8_t enabled;
    uint8_t has_sentence;
    uint8_t has_fix;
    uint8_t source;
    uint8_t local_recorded;
    int last_parse_ret;
    uint32_t rx_bytes;
    uint32_t rx_chunks;
    uint32_t line_count;
    uint32_t valid_sentences;
    uint32_t fix_sentences;
    uint32_t no_fix_sentences;
    uint32_t format_errors;
    uint32_t overflow_errors;
    uint32_t unsupported_sentences;
    uint32_t last_rx_ms;
    uint32_t last_sentence_ms;
    uint32_t last_fix_ms;
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
        g_gps.format_errors++;
        return;
    }
    g_gps.rx_chunks++;
    g_gps.rx_bytes += (uint32_t)length;
    g_gps.last_rx_ms = (uint32_t)uapi_tcxo_get_ms();
    /* UART callback receives arbitrary byte chunks, not complete NMEA lines.
     * Feed bytes one at a time; the parser returns SLE_TEAM_OK only when a
     * complete supported sentence has produced a position body. */
    for (i = 0U; i < length; i++) {
        sle_team_pos_body_t pos = {0};
        size_t before_len = g_gps.line_len;
        int ret = sle_team_nmea_feed(&g_gps.nmea, (char)data[i], g_gps.line_buf,
            sizeof(g_gps.line_buf), &g_gps.line_len, &pos);

        if ((data[i] == (uint8_t)'\r' || data[i] == (uint8_t)'\n') && before_len != 0U) {
            g_gps.line_count++;
            g_gps.last_sentence_ms = (uint32_t)uapi_tcxo_get_ms();
        }
        if (ret == SLE_TEAM_OK) {
            g_gps.valid_sentences++;
            g_gps.has_sentence = 1U;
            g_gps.last_parse_ret = ret;
            g_gps.last_pos = pos;
            g_gps.source = WS63_TEAM_GPS_SOURCE_NMEA;
            g_gps.local_recorded = 0U;
            g_gps.has_fix = pos.fix_status != 0U ? 1U : 0U;
            if (pos.fix_status != 0U) {
                g_gps.fix_sentences++;
                g_gps.last_fix_ms = (uint32_t)uapi_tcxo_get_ms();
            } else {
                g_gps.no_fix_sentences++;
            }
        } else if (ret == SLE_TEAM_ERR_BUF) {
            g_gps.overflow_errors++;
            g_gps.last_parse_ret = ret;
        } else if (ret == SLE_TEAM_ERR_UNSUPPORTED) {
            g_gps.unsupported_sentences++;
            g_gps.has_sentence = 1U;
            g_gps.last_parse_ret = ret;
        } else if ((data[i] == (uint8_t)'\r' || data[i] == (uint8_t)'\n') &&
            before_len != 0U && ret != SLE_TEAM_ERR_FORMAT) {
            g_gps.last_parse_ret = ret;
        } else if ((data[i] == (uint8_t)'\r' || data[i] == (uint8_t)'\n') &&
            before_len != 0U && ret == SLE_TEAM_ERR_FORMAT) {
            g_gps.format_errors++;
            g_gps.last_parse_ret = ret;
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
    g_gps.enabled = 1U;
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
    /*
     * First update the board-local record so the AP can show GPS before join.
     * Only the second half sends upstream telemetry, and only for joined members.
     */
    if (node == NULL || g_gps.ready == 0U || g_gps.has_sentence == 0U) {
        return;
    }
    g_gps.last_pos.battery_percent = battery_percent;
    if (g_gps.local_recorded == 0U) {
        if (sle_team_node_record_local_position(node, &g_gps.last_pos) == SLE_TEAM_OK) {
            g_gps.local_recorded = 1U;
        }
    }
    if (g_gps.has_fix == 0U || node->cfg.role != SLE_TEAM_ROLE_MEMBER || node->joined == 0U ||
        node->cfg.report_interval_s == 0U) {
        return;
    }
    if (g_gps.last_report_ms != 0U &&
        (uint32_t)(now_ms - g_gps.last_report_ms) < (uint32_t)node->cfg.report_interval_s * 1000U) {
        return;
    }
    g_gps.last_report_ms = now_ms;
    (void)sle_team_node_send_position(node, node->cfg.leader_id, &g_gps.last_pos);
}

void ws63_team_gps_get_status(ws63_team_gps_status_t *status)
{
    if (status == NULL) {
        return;
    }
    (void)memset_s(status, sizeof(*status), 0, sizeof(*status));
    status->enabled = g_gps.enabled;
    status->ready = g_gps.ready;
    status->has_sentence = g_gps.has_sentence;
    status->has_fix = g_gps.has_fix;
    status->source = g_gps.source;
    status->last_fix_status = g_gps.last_pos.fix_status;
    status->last_sat_count = g_gps.last_pos.sat_count;
    status->last_parse_ret = g_gps.last_parse_ret;
    status->rx_bytes = g_gps.rx_bytes;
    status->rx_chunks = g_gps.rx_chunks;
    status->line_count = g_gps.line_count;
    status->valid_sentences = g_gps.valid_sentences;
    status->fix_sentences = g_gps.fix_sentences;
    status->no_fix_sentences = g_gps.no_fix_sentences;
    status->format_errors = g_gps.format_errors;
    status->overflow_errors = g_gps.overflow_errors;
    status->unsupported_sentences = g_gps.unsupported_sentences;
    status->last_rx_ms = g_gps.last_rx_ms;
    status->last_sentence_ms = g_gps.last_sentence_ms;
    status->last_fix_ms = g_gps.last_fix_ms;
    status->latitude_e6 = g_gps.last_pos.latitude_e6;
    status->longitude_e6 = g_gps.last_pos.longitude_e6;
    status->speed_cms = g_gps.last_pos.speed_cms;
    status->heading_deg = g_gps.last_pos.heading_deg;
}

void ws63_team_gps_set_fallback_position(const sle_team_pos_body_t *pos, uint32_t now_ms)
{
    if (pos == NULL) {
        return;
    }
    g_gps.last_pos = *pos;
    g_gps.has_sentence = 1U;
    g_gps.has_fix = pos->fix_status != 0U ? 1U : 0U;
    g_gps.source = WS63_TEAM_GPS_SOURCE_FALLBACK;
    g_gps.local_recorded = 0U;
    g_gps.last_sentence_ms = now_ms;
    if (pos->fix_status != 0U) {
        g_gps.last_fix_ms = now_ms;
    }
}
