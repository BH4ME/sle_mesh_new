#include "sle_team_nmea.h"

#include <ctype.h>
#include <string.h>

static uint8_t nmea_valid_sentence(const char *line)
{
    const char *star;
    const char *p;
    uint8_t checksum = 0U;
    uint8_t expected = 0U;
    int i;

    if (line == NULL || line[0] != '$') {
        return 0U;
    }
    star = strchr(line, '*');
    if (star == NULL || star[1] == '\0' || star[2] == '\0') {
        return 0U;
    }
    p = line + 1;
    while (*p != '\0' && p != star) {
        checksum ^= (uint8_t)*p;
        p++;
    }
    for (i = 1; i <= 2; i++) {
        char c = (char)toupper((unsigned char)star[i]);
        expected <<= 4U;
        if (c >= '0' && c <= '9') {
            expected |= (uint8_t)(c - '0');
        } else if (c >= 'A' && c <= 'F') {
            expected |= (uint8_t)(10 + c - 'A');
        } else {
            return 0U;
        }
    }
    return checksum == expected ? 1U : 0U;
}

static uint8_t nmea_field_copy(const char *start, const char *end, char *out, size_t out_len)
{
    size_t len;

    if (start == NULL || end == NULL || out == NULL || out_len == 0U || end < start) {
        return 0U;
    }
    len = (size_t)(end - start);
    if (len >= out_len) {
        return 0U;
    }
    (void)memcpy(out, start, len);
    out[len] = '\0';
    return 1U;
}

static int nmea_parse_int(const char *text, int fallback)
{
    int sign = 1;
    int value = 0;

    if (text == NULL || *text == '\0') {
        return fallback;
    }
    if (*text == '-') {
        sign = -1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    if (*text == '\0') {
        return fallback;
    }
    while (*text != '\0') {
        if (*text < '0' || *text > '9') {
            return fallback;
        }
        value = value * 10 + (*text - '0');
        text++;
    }
    return value * sign;
}

static int32_t nmea_parse_decimal_scaled(const char *text, int32_t scale, int32_t fallback)
{
    int32_t sign = 1;
    int32_t value = 0;
    int32_t frac_scale = scale;
    uint8_t saw_digit = 0U;
    uint8_t after_dot = 0U;

    if (text == NULL || *text == '\0' || scale <= 0) {
        return fallback;
    }
    if (*text == '-') {
        sign = -1;
        text++;
    } else if (*text == '+') {
        text++;
    }
    while (*text != '\0') {
        if (*text == '.') {
            if (after_dot != 0U) {
                return fallback;
            }
            after_dot = 1U;
            text++;
            continue;
        }
        if (*text < '0' || *text > '9') {
            return fallback;
        }
        saw_digit = 1U;
        if (after_dot == 0U) {
            value = value * 10 + (int32_t)(*text - '0') * scale;
        } else if (frac_scale > 1) {
            frac_scale /= 10;
            value += (int32_t)(*text - '0') * frac_scale;
        }
        text++;
    }
    return saw_digit != 0U ? value * sign : fallback;
}

static uint8_t nmea_parse_u8(const char *text, uint8_t fallback)
{
    int value = nmea_parse_int(text, -1);
    return value < 0 ? fallback : (uint8_t)value;
}

static uint16_t nmea_abs_u16(int value)
{
    return (uint16_t)(value < 0 ? -value : value);
}

static int32_t nmea_parse_coord_e6(const char *coord, const char *hemisphere)
{
    const char *dot;
    char deg_buf[4];
    char min_buf[16];
    int deg;
    int32_t minutes_e6;
    int sign = 1;
    size_t dot_index;
    size_t coord_len;

    if (coord == NULL || hemisphere == NULL || coord[0] == '\0' || hemisphere[0] == '\0') {
        return 0;
    }
    dot = strchr(coord, '.');
    if (dot == NULL || dot - coord < 3) {
        return 0;
    }
    dot_index = (size_t)(dot - coord);
    coord_len = strlen(coord);
    if (dot_index < 2U) {
        return 0;
    }
    if (!nmea_field_copy(coord, coord + dot_index - 2U, deg_buf, sizeof(deg_buf)) ||
        !nmea_field_copy(coord + dot_index - 2U, coord + coord_len, min_buf, sizeof(min_buf))) {
        return 0;
    }
    deg = nmea_parse_int(deg_buf, 0);
    minutes_e6 = nmea_parse_decimal_scaled(min_buf, 1000000L, 0);
    if (hemisphere[0] == 'S' || hemisphere[0] == 'W') {
        sign = -1;
    }
    return (int32_t)(sign * ((int32_t)deg * 1000000L + minutes_e6 / 60L));
}

static uint16_t nmea_parse_speed_cms(const char *text)
{
    int32_t knots10 = nmea_parse_decimal_scaled(text, 10, -1);

    if (knots10 < 0) {
        return 0U;
    }
    return (uint16_t)((knots10 * 514444L + 50000L) / 100000L);
}

static uint16_t nmea_parse_heading_deg(const char *text)
{
    int32_t heading10 = nmea_parse_decimal_scaled(text, 10, -1);

    if (heading10 < 0) {
        return 0U;
    }
    return nmea_abs_u16(heading10) / 10U;
}

static void nmea_commit_fix_from_gga(sle_team_nmea_state_t *state, char **field)
{
    if (state == NULL || field == NULL) {
        return;
    }
    state->pos.fix_status = nmea_parse_u8(field[6], 0U);
    state->pos.sat_count = nmea_parse_u8(field[7], 0U);
    state->have_gga = 1U;
}

static void nmea_commit_rmc(sle_team_nmea_state_t *state, char **field)
{
    uint8_t status;

    if (state == NULL || field == NULL) {
        return;
    }
    status = (uint8_t)((field[2] != NULL && field[2][0] == 'A') ? 1U : 0U);
    if (status == 0U) {
        state->pos.fix_status = 0U;
        state->have_rmc = 1U;
        return;
    }
    state->pos.latitude_e6 = nmea_parse_coord_e6(field[3], field[4]);
    state->pos.longitude_e6 = nmea_parse_coord_e6(field[5], field[6]);
    state->pos.speed_cms = nmea_parse_speed_cms(field[7]);
    state->pos.heading_deg = nmea_parse_heading_deg(field[8]);
    state->pos.fix_status = 1U;
    state->have_rmc = 1U;
}

void sle_team_nmea_init(sle_team_nmea_state_t *state)
{
    if (state == NULL) {
        return;
    }
    (void)memset(state, 0, sizeof(*state));
}

int sle_team_nmea_parse_line(sle_team_nmea_state_t *state, const char *line, sle_team_pos_body_t *out)
{
    char sentence[96];
    char *fields[16];
    size_t field_count = 0U;
    char *cursor;
    char *star;
    char *p;

    if (state == NULL || line == NULL || out == NULL) {
        return SLE_TEAM_ERR_ARG;
    }
    (void)memset(fields, 0, sizeof(fields));
    if (strlen(line) >= sizeof(sentence)) {
        return SLE_TEAM_ERR_BUF;
    }
    (void)memset(sentence, 0, sizeof(sentence));
    (void)memcpy(sentence, line, strlen(line));
    if (nmea_valid_sentence(sentence) == 0U) {
        return SLE_TEAM_ERR_FORMAT;
    }
    star = strchr(sentence, '*');
    if (star != NULL) {
        *star = '\0';
    }
    cursor = sentence;
    if (*cursor == '$') {
        cursor++;
    }
    p = cursor;
    while (p != NULL && field_count < (sizeof(fields) / sizeof(fields[0]))) {
        fields[field_count++] = p;
        p = strchr(p, ',');
        if (p != NULL) {
            *p = '\0';
            p++;
        }
    }
    if (field_count == 0U) {
        return SLE_TEAM_ERR_FORMAT;
    }
    if (strncmp(fields[0], "GPRMC", 5) == 0 || strncmp(fields[0], "GNRMC", 5) == 0) {
        if (field_count < 9U) {
            return SLE_TEAM_ERR_FORMAT;
        }
        nmea_commit_rmc(state, fields);
    } else if (strncmp(fields[0], "GPGGA", 5) == 0 || strncmp(fields[0], "GNGGA", 5) == 0) {
        if (field_count < 8U) {
            return SLE_TEAM_ERR_FORMAT;
        }
        nmea_commit_fix_from_gga(state, fields);
    } else {
        return SLE_TEAM_ERR_UNSUPPORTED;
    }
    *out = state->pos;
    return SLE_TEAM_OK;
}

int sle_team_nmea_feed(sle_team_nmea_state_t *state, char ch, char *line_buf, size_t line_buf_len,
    size_t *line_len, sle_team_pos_body_t *out)
{
    if (state == NULL || line_buf == NULL || line_len == NULL || out == NULL || line_buf_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    if (ch == '\r' || ch == '\n') {
        int ret;

        if (*line_len == 0U) {
            return SLE_TEAM_ERR_FORMAT;
        }
        line_buf[*line_len] = '\0';
        ret = sle_team_nmea_parse_line(state, line_buf, out);
        *line_len = 0U;
        line_buf[0] = '\0';
        return ret;
    }
    if (*line_len + 1U >= line_buf_len) {
        *line_len = 0U;
        line_buf[0] = '\0';
        return SLE_TEAM_ERR_BUF;
    }
    line_buf[*line_len] = ch;
    (*line_len)++;
    return SLE_TEAM_ERR_FORMAT;
}
