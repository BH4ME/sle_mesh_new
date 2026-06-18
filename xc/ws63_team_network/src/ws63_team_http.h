#ifndef WS63_TEAM_HTTP_H
#define WS63_TEAM_HTTP_H

#include "sle_team_node.h"
#include "sle_team_web_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char self_label[16];
    char leader_label[16];
    char softap_ssid[32];
    uint8_t self_mac[6];
    uint8_t self_mac_ready;
    uint8_t route_id;
    uint16_t self_suffix;
    uint8_t role_configured;
    uint8_t role_request_pending;
    uint8_t role_request_role;
    uint8_t role_request_team;
    uint8_t role_request_channel;
    uint8_t role_request_leader;
    uint16_t role_request_leader_suffix;
    uint16_t role_request_leader_term;
    int role_request_last_ret;
    const char *fw_version;
} ws63_team_http_identity_t;

typedef struct {
    void (*get_identity)(ws63_team_http_identity_t *identity);
    int (*write_config_status_json)(char *out, size_t out_len);
    int (*save_leader)(uint8_t team_id, uint8_t channel_hash, uint16_t leader_term, uint8_t apply_now);
    int (*save_member)(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash,
        uint16_t leader_term, uint8_t apply_now);
    int (*apply_saved)(void);
    int (*clear_config)(void);
    void (*reboot)(const char *reason);
    int (*pairing_start)(void);
    int (*pairing_stop)(void);
    int (*pairing_approve)(uint8_t member_id, uint8_t relay_allowed);
    int (*member_select)(uint16_t leader_suffix, uint8_t team_id, uint8_t channel_hash);
    int (*member_leave)(void);
    int (*factory_reset)(void);
} ws63_team_http_callbacks_t;

/*
 * SoftAP HTTP surface for the board-local Web UI.
 *
 * The HTTP task owns only SoftAP, socket handling, tiny SSR pages and GET
 * request parsing. Role changes and pairing control are delegated through the
 * callback table so the WS63 app can keep SLE state changes on its own task.
 */
void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events,
    const ws63_team_http_callbacks_t *callbacks, const char *ssid);

#ifdef __cplusplus
}
#endif

#endif
