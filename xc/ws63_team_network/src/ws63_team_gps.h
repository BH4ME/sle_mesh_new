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
void ws63_team_gps_init(void);
void ws63_team_gps_tick(sle_team_node_t *node, uint32_t now_ms, uint8_t battery_percent);

#ifdef __cplusplus
}
#endif

#endif
