#ifndef WS63_TEAM_HTTP_H
#define WS63_TEAM_HTTP_H

#include "sle_team_node.h"
#include "sle_team_web_api.h"

#ifdef __cplusplus
extern "C" {
#endif

void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events, const char *ssid);

#ifdef __cplusplus
}
#endif

#endif
