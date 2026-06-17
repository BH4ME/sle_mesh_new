#ifndef WS63_TEAM_GPS_H
#define WS63_TEAM_GPS_H

#include "sle_team_node.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws63_team_gps_init(void);
void ws63_team_gps_tick(sle_team_node_t *node, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
