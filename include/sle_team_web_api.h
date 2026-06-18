#ifndef SLE_TEAM_WEB_API_H
#define SLE_TEAM_WEB_API_H

#include "sle_team_node.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tiny JSON API shared by the board HTTP server and desktop/Web tests.
 *
 * The functions write into caller-provided buffers because the firmware side
 * avoids dynamic allocation in the HTTP path. Return value is the byte count or
 * SLE_TEAM_ERR_BUF if the JSON was truncated.
 */
#define SLE_TEAM_WEB_EVENT_COUNT 8U
#define SLE_TEAM_WEB_EVENT_SUMMARY_SIZE 64U

/* Direction labels for recent packet/system events shown by the Web UI. */
typedef enum {
    SLE_TEAM_WEB_EVENT_RX = 0,
    SLE_TEAM_WEB_EVENT_TX = 1,
    SLE_TEAM_WEB_EVENT_SYSTEM = 2,
} sle_team_web_event_direction_t;

/* One compact event row; summary is intentionally short for MCU RAM limits. */
typedef struct {
    uint32_t id;
    uint32_t time_s;
    sle_team_web_event_direction_t direction;
    uint8_t app_msg_type;
    uint8_t src_id;
    uint8_t dst_id;
    uint16_t seq;
    char summary[SLE_TEAM_WEB_EVENT_SUMMARY_SIZE];
} sle_team_web_event_t;

/* Ring buffer: newest event overwrites the oldest once count reaches 8. */
typedef struct {
    sle_team_web_event_t events[SLE_TEAM_WEB_EVENT_COUNT];
    uint32_t next_id;
    uint8_t head;
    uint8_t count;
} sle_team_web_event_log_t;

void sle_team_web_event_log_init(sle_team_web_event_log_t *log);
void sle_team_web_event_push(sle_team_web_event_log_t *log, uint32_t time_s,
    sle_team_web_event_direction_t direction, uint8_t app_msg_type, uint8_t src_id, uint8_t dst_id, uint16_t seq,
    const char *summary);

/* /api/status, /api/nodes, /api/pending, and /api/events JSON writers. */
int sle_team_web_write_status_json(const sle_team_node_t *node, uint32_t uptime_s, const char *transport,
    char *out, size_t out_len);
int sle_team_web_write_nodes_json(const sle_team_node_t *node, char *out, size_t out_len);
int sle_team_web_write_pending_json(const sle_team_node_t *node, char *out, size_t out_len);
int sle_team_web_write_events_json(const sle_team_web_event_log_t *log, char *out, size_t out_len);

const char *sle_team_web_role_name(uint8_t role);
const char *sle_team_web_state_name(uint8_t state);
const char *sle_team_web_parent_state_name(uint8_t state);
const char *sle_team_web_msg_type_name(uint8_t app_msg_type);

#ifdef __cplusplus
}
#endif

#endif
