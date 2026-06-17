#ifndef SLE_TEAM_NMEA_H
#define SLE_TEAM_NMEA_H

#include "sle_team_packet.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    sle_team_pos_body_t pos;
    uint8_t have_rmc;
    uint8_t have_gga;
} sle_team_nmea_state_t;

void sle_team_nmea_init(sle_team_nmea_state_t *state);
int sle_team_nmea_parse_line(sle_team_nmea_state_t *state, const char *line, sle_team_pos_body_t *out);
int sle_team_nmea_feed(sle_team_nmea_state_t *state, char ch, char *line_buf, size_t line_buf_len,
    size_t *line_len, sle_team_pos_body_t *out);

#ifdef __cplusplus
}
#endif

#endif
