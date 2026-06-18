#ifndef WS63_TEAM_GPS_H
#define WS63_TEAM_GPS_H

#include "sle_team_node.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Board GPS service for L80R/L86-style NMEA modules.
 *
 * init() opens the configured UART path. tick() feeds the latest valid fix into
 * the portable team node so the leader/Web UI can retain last-known positions.
 */
typedef struct {
    uint8_t enabled;
    uint8_t ready;
    uint8_t has_sentence;
    uint8_t has_fix;
    uint8_t source; /* 0 none, 1 hardware NMEA, 2 phone/Web fallback. */
    uint8_t last_fix_status;
    uint8_t last_sat_count;
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
    int32_t latitude_e6;
    int32_t longitude_e6;
    uint16_t speed_cms;
    uint16_t heading_deg;
} ws63_team_gps_status_t;

void ws63_team_gps_init(void);
void ws63_team_gps_tick(sle_team_node_t *node, uint32_t now_ms, uint8_t battery_percent);
void ws63_team_gps_get_status(ws63_team_gps_status_t *status);
void ws63_team_gps_set_fallback_position(const sle_team_pos_body_t *pos, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
