#ifndef SLE_TEAM_NMEA_H
#define SLE_TEAM_NMEA_H

#include "sle_team_packet.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Minimal NMEA parser for GPS modules such as L80R/L86.
 *
 * The parser accepts a byte stream, assembles CR/LF-terminated lines, and
 * updates sle_team_pos_body_t from valid RMC/GGA sentences. Coordinates are
 * converted to E6 integer degrees for the firmware packet format.
 */
typedef struct {
    sle_team_pos_body_t pos;
    uint8_t have_rmc;
    uint8_t have_gga;
} sle_team_nmea_state_t;

void sle_team_nmea_init(sle_team_nmea_state_t *state);
/* Parse one full NMEA line; returns SLE_TEAM_OK when a usable position changed. */
int sle_team_nmea_parse_line(sle_team_nmea_state_t *state, const char *line, sle_team_pos_body_t *out);
/* Feed one UART byte; line_buf and line_len are owned by the caller. */
int sle_team_nmea_feed(sle_team_nmea_state_t *state, char ch, char *line_buf, size_t line_buf_len,
    size_t *line_len, sle_team_pos_body_t *out);

#ifdef __cplusplus
}
#endif

#endif
