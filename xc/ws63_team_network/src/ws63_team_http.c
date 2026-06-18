#include "ws63_team_http.h"

#if defined(CONFIG_SLE_TEAM_WIFI_AP_ENABLE)

#include "common_def.h"
#include "errcode.h"
#include "lwip/netifapi.h"
#include "lwip/sockets.h"
#include "securec.h"
#include "soc_osal.h"
#include "tcxo.h"
#include "wifi_device.h"
#include "wifi_device_config.h"
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"
#include "ws63_console_pages.h"
#include "ws63_team_gps.h"

#include <errno.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_SLE_TEAM_WIFI_AP_SSID
#define CONFIG_SLE_TEAM_WIFI_AP_SSID "SLE"
#endif
#ifndef CONFIG_SLE_TEAM_WIFI_AP_PSK
#define CONFIG_SLE_TEAM_WIFI_AP_PSK "123456789"
#endif
#ifndef CONFIG_SLE_TEAM_WIFI_AP_CHANNEL
#define CONFIG_SLE_TEAM_WIFI_AP_CHANNEL 6
#endif
#ifndef CONFIG_SLE_TEAM_WIFI_AP_IP_LAST
#define CONFIG_SLE_TEAM_WIFI_AP_IP_LAST 1
#endif

#define TEAM_HTTP_TASK_STACK_SIZE 0x1800
#define TEAM_HTTP_TASK_PRIO 29
#define TEAM_HTTP_PORT 80
#define TEAM_HTTP_BACKLOG 4
#define TEAM_HTTP_REQ_SIZE 768
#define TEAM_HTTP_JSON_SIZE 2048
#define TEAM_HTTP_HTML_SIZE 8192
#define TEAM_HTTP_PATH_SIZE 160
#define TEAM_HTTP_WIFI_WAIT_MS 10000U
#define TEAM_HTTP_RETRY_MS 5000U
#define TEAM_HTTP_START_DELAY_MS 5000U
#define TEAM_HTTP_RECV_TIMEOUT_MS 800U
#define TEAM_HTTP_SEND_TIMEOUT_MS 3000U
#define TEAM_HTTP_WIFI_SECURITY_PRIMARY WIFI_SEC_TYPE_WPA2PSK
#define TEAM_HTTP_WIFI_PROTOCOL_PRIMARY WIFI_MODE_11B_G_N
#define TEAM_HTTP_WIFI_SECURITY_COMPAT_MIX ((wifi_security_enum)WIFI_SEC_TYPE_WPA2_WPA_PSK_MIX)
#define TEAM_HTTP_WIFI_PROTOCOL_COMPAT_AX WIFI_MODE_11B_G_N_AX

typedef struct {
    sle_team_node_t *node;
    sle_team_web_event_log_t *events;
    ws63_team_http_callbacks_t cb;
    char ssid[32];
    int listen_fd;
    int last_errno;
    uint32_t accept_count;
    uint8_t ready;
    uint8_t started;
} team_http_ctx_t;

static team_http_ctx_t g_team_http;
static char g_team_http_req[TEAM_HTTP_REQ_SIZE];
static char g_team_http_json[TEAM_HTTP_JSON_SIZE];
static char g_team_http_html[TEAM_HTTP_HTML_SIZE];

static uint32_t team_http_now_s(void)
{
    return (uint32_t)(uapi_tcxo_get_ms() / 1000U);
}

static void team_http_get_identity(ws63_team_http_identity_t *identity)
{
    if (identity == NULL) {
        return;
    }
    (void)memset_s(identity, sizeof(*identity), 0, sizeof(*identity));
    if (g_team_http.cb.get_identity != NULL) {
        g_team_http.cb.get_identity(identity);
    }
    if (identity->softap_ssid[0] == '\0') {
        (void)snprintf(identity->softap_ssid, sizeof(identity->softap_ssid), "%s", g_team_http.ssid);
    }
    if (identity->self_label[0] == '\0') {
        (void)snprintf(identity->self_label, sizeof(identity->self_label), "U%02X", identity->route_id);
    }
    if (identity->leader_label[0] == '\0') {
        (void)snprintf(identity->leader_label, sizeof(identity->leader_label), "L%02X",
            g_team_http.node != NULL ? g_team_http.node->cfg.leader_id : 0U);
    }
}

static uint8_t team_http_default_team(void)
{
    return g_team_http.node != NULL && g_team_http.node->cfg.team_id != 0U ?
        g_team_http.node->cfg.team_id : 1U;
}

static uint8_t team_http_default_channel(void)
{
    return g_team_http.node != NULL ? g_team_http.node->cfg.channel_hash : 17U;
}

static uint8_t team_http_route_id_from_suffix(uint16_t suffix)
{
    uint16_t mix;

    if (suffix == 0U) {
        return 1U;
    }
    mix = (uint16_t)((suffix & 0xFFU) + (((suffix >> 8U) & 0xFFU) * 31U));
    return (uint8_t)((mix % 254U) + 1U);
}

static const char *team_http_role_name(uint8_t role, uint8_t valid)
{
    if (valid == 0U) {
        return "none";
    }
    return role == (uint8_t)SLE_TEAM_ROLE_LEADER ? "leader" : "member";
}

static void team_http_format_route_label(uint8_t node_id, uint8_t role, const uint8_t mac[6],
    uint8_t mac_ready, char *out, size_t out_len)
{
    char prefix = role == (uint8_t)SLE_TEAM_ROLE_LEADER ? 'L' : 'M';

    if (out == NULL || out_len == 0U) {
        return;
    }
    if (node_id == SLE_TEAM_BROADCAST_ID) {
        (void)snprintf(out, out_len, "ALL");
        return;
    }
    if (mac_ready != 0U && mac != NULL) {
        (void)snprintf(out, out_len, "%c%02X%02X", prefix, mac[4], mac[5]);
        return;
    }
    (void)snprintf(out, out_len, "%c%02X", prefix, node_id);
}

static uint8_t team_http_online_node_count(void)
{
    uint8_t i;
    uint8_t count = 0U;

    if (g_team_http.node == NULL) {
        return 0U;
    }
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER || g_team_http.node->joined != 0U) {
        count = 1U;
    }
    if (g_team_http.node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return count;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_http.node->members[i];
        if (member->online != 0U) {
            count++;
        }
    }
    return count;
}

static uint8_t team_http_relay_node_count(void)
{
    uint8_t i;
    uint8_t count = 0U;

    if (g_team_http.node == NULL) {
        return 0U;
    }
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER &&
        (g_team_http.node->cfg.relay_enabled != 0U || g_team_http.node->cfg.relay_allowed != 0U)) {
        return 1U;
    }
    if (g_team_http.node->cfg.role != SLE_TEAM_ROLE_LEADER) {
        return 0U;
    }
    for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
        const sle_team_member_record_t *member = &g_team_http.node->members[i];
        if (member->online != 0U && member->relay_allowed != 0U) {
            count++;
        }
    }
    return count;
}

static int team_http_send_all(int fd, const char *data, size_t len)
{
    size_t sent = 0U;

    while (sent < len) {
        int ret = send(fd, data + sent, len - sent, 0);
        if (ret <= 0) {
            g_team_http.last_errno = errno;
            osal_printk("[team-wifi] http send failed fd=%d ret=%d errno=%d sent=%u/%u\r\n",
                fd, ret, g_team_http.last_errno, (unsigned int)sent, (unsigned int)len);
            return -1;
        }
        sent += (size_t)ret;
    }
    return 0;
}

static void team_http_set_client_timeouts(int fd)
{
    struct timeval recv_timeout = {
        .tv_sec = TEAM_HTTP_RECV_TIMEOUT_MS / 1000U,
        .tv_usec = (TEAM_HTTP_RECV_TIMEOUT_MS % 1000U) * 1000U,
    };
    struct timeval send_timeout = {
        .tv_sec = TEAM_HTTP_SEND_TIMEOUT_MS / 1000U,
        .tv_usec = (TEAM_HTTP_SEND_TIMEOUT_MS % 1000U) * 1000U,
    };

    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &recv_timeout, sizeof(recv_timeout));
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &send_timeout, sizeof(send_timeout));
}

static int team_http_recv_request_line(int fd)
{
    size_t used = 0U;
    size_t cap = sizeof(g_team_http_req) - 1U;

    while (used < cap) {
        int ret = recv(fd, g_team_http_req + used, cap - used, 0);
        if (ret < 0) {
            g_team_http.last_errno = errno;
            if (used == 0U) {
                osal_printk("[team-wifi] http recv failed fd=%d ret=%d errno=%d\r\n",
                    fd, ret, g_team_http.last_errno);
                return -1;
            }
            break;
        }
        if (ret == 0) {
            return used == 0U ? -1 : (int)used;
        }
        used += (size_t)ret;
        g_team_http_req[used] = '\0';
        if (strstr(g_team_http_req, "\r\n") != NULL || strchr(g_team_http_req, '\n') != NULL) {
            break;
        }
    }
    return used == 0U ? -1 : (int)used;
}

static void team_http_send_response(int fd, const char *status, const char *content_type, const char *body)
{
    char header[320];
    size_t body_len = body != NULL ? strlen(body) : 0U;
    int header_len;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %u\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "\r\n",
        status, content_type, (unsigned int)body_len);
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        (void)team_http_send_all(fd, header, (size_t)header_len);
    }
    if (body_len > 0U) {
        (void)team_http_send_all(fd, body, body_len);
    }
}

static void team_http_send_redirect(int fd, const char *location)
{
    char header[320];
    int header_len;

    header_len = snprintf(header, sizeof(header),
        "HTTP/1.1 303 See Other\r\n"
        "Location: %s\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n"
        "Access-Control-Allow-Origin: *\r\n"
        "Cache-Control: no-store, no-cache, must-revalidate\r\n"
        "Pragma: no-cache\r\n"
        "Expires: 0\r\n"
        "\r\n",
        location != NULL ? location : "/");
    if (header_len > 0 && header_len < (int)sizeof(header)) {
        (void)team_http_send_all(fd, header, (size_t)header_len);
    }
}

static void team_http_send_json(int fd, const char *body)
{
    team_http_send_response(fd, "200 OK", "application/json", body != NULL ? body : "{}");
}

static int team_http_write_gps_json(char *out, size_t out_len)
{
    ws63_team_gps_status_t gps;
    int len;

    if (out == NULL || out_len == 0U) {
        return SLE_TEAM_ERR_ARG;
    }
    ws63_team_gps_get_status(&gps);
    len = snprintf(out, out_len,
        "{\"enabled\":%s,\"ready\":%s,\"hasSentence\":%s,\"hasFix\":%s,\"source\":%u,"
        "\"fixStatus\":%u,\"satCount\":%u,\"lastParseRet\":%d,"
        "\"rxBytes\":%lu,\"rxChunks\":%lu,\"lineCount\":%lu,\"validSentences\":%lu,"
        "\"fixSentences\":%lu,\"noFixSentences\":%lu,\"formatErrors\":%lu,"
        "\"overflowErrors\":%lu,\"unsupportedSentences\":%lu,"
        "\"lastRxMs\":%lu,\"lastSentenceMs\":%lu,\"lastFixMs\":%lu,"
        "\"latitudeE6\":%ld,\"longitudeE6\":%ld,\"speedCms\":%u,\"headingDeg\":%u}",
        gps.enabled != 0U ? "true" : "false",
        gps.ready != 0U ? "true" : "false",
        gps.has_sentence != 0U ? "true" : "false",
        gps.has_fix != 0U ? "true" : "false",
        gps.source, gps.last_fix_status, gps.last_sat_count, gps.last_parse_ret,
        (unsigned long)gps.rx_bytes, (unsigned long)gps.rx_chunks,
        (unsigned long)gps.line_count, (unsigned long)gps.valid_sentences,
        (unsigned long)gps.fix_sentences, (unsigned long)gps.no_fix_sentences,
        (unsigned long)gps.format_errors, (unsigned long)gps.overflow_errors,
        (unsigned long)gps.unsupported_sentences, (unsigned long)gps.last_rx_ms,
        (unsigned long)gps.last_sentence_ms, (unsigned long)gps.last_fix_ms,
        (long)gps.latitude_e6, (long)gps.longitude_e6, gps.speed_cms, gps.heading_deg);
    if (len < 0 || len >= (int)out_len) {
        return SLE_TEAM_ERR_BUF;
    }
    return len;
}

static void team_http_send_gps_json_response(int fd)
{
    int ret = team_http_write_gps_json(g_team_http_json, sizeof(g_team_http_json));
    team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
        ret < 0 ? "{\"ok\":false,\"error\":\"gps\"}" : g_team_http_json);
}

static void team_http_send_bad_request(int fd, const char *error)
{
    int len = snprintf(g_team_http_json, sizeof(g_team_http_json),
        "{\"ok\":false,\"error\":\"%s\"}", error != NULL ? error : "bad-request");
    team_http_send_response(fd, len < 0 || len >= (int)sizeof(g_team_http_json) ?
        "500 Internal Server Error" : "400 Bad Request", "application/json",
        len < 0 || len >= (int)sizeof(g_team_http_json) ?
            "{\"ok\":false,\"error\":\"bad-request\"}" : g_team_http_json);
}

static void team_http_get_path(char *out, size_t out_size)
{
    char *start;
    char *end;
    size_t len;

    if (out == NULL || out_size == 0U) {
        return;
    }
    out[0] = '\0';
    start = strchr(g_team_http_req, ' ');
    if (start == NULL) {
        return;
    }
    start++;
    end = strchr(start, ' ');
    if (end == NULL || end <= start) {
        return;
    }
    len = (size_t)(end - start);
    if (len >= out_size) {
        len = out_size - 1U;
    }
    (void)memcpy_s(out, out_size, start, len);
    out[len] = '\0';
}

static const char *team_http_query_value_start(const char *path, const char *key)
{
    char pattern[24];
    const char *p;

    if (path == NULL || key == NULL) {
        return NULL;
    }
    (void)snprintf(pattern, sizeof(pattern), "%s=", key);
    p = strstr(path, pattern);
    return p == NULL ? NULL : p + strlen(pattern);
}

static int team_http_query_number(const char *path, const char *key, int64_t min_value, int64_t max_value,
    unsigned long positive_limit, unsigned long negative_limit, int64_t *out)
{
    const char *p;
    unsigned long limit;
    uint8_t negative = 0U;
    unsigned long abs_value = 0UL;
    uint8_t digits = 0U;
    int64_t signed_value;

    if (out == NULL) {
        return -1;
    }
    p = team_http_query_value_start(path, key);
    if (p == NULL) {
        return -1;
    }
    if (negative_limit != 0UL && *p == '-') {
        negative = 1U;
        p++;
    } else if (negative_limit != 0UL && *p == '+') {
        p++;
    }
    limit = negative != 0U ? negative_limit : positive_limit;
    while (*p >= '0' && *p <= '9') {
        abs_value = abs_value * 10UL + (unsigned long)(*p - '0');
        p++;
        digits++;
        if (abs_value > limit) {
            return -1;
        }
    }
    if (*p != '\0' && *p != '&') {
        return -1;
    }
    if (digits == 0U) {
        return -1;
    }
    signed_value = negative != 0U ? -(int64_t)abs_value : (int64_t)abs_value;
    if (signed_value < min_value || signed_value > max_value) {
        return -1;
    }
    *out = signed_value;
    return 0;
}

static int team_http_query_u8(const char *path, const char *key, uint8_t min_value, uint8_t max_value,
    uint8_t *out)
{
    int64_t value;
    if (team_http_query_number(path, key, (int64_t)min_value, (int64_t)max_value, 255UL, 0UL, &value) != 0 ||
        out == NULL) {
        return -1;
    }
    *out = (uint8_t)value;
    return 0;
}

static int team_http_query_u16(const char *path, const char *key, uint16_t min_value, uint16_t max_value,
    uint16_t *out)
{
    int64_t value;
    if (team_http_query_number(path, key, (int64_t)min_value, (int64_t)max_value, 65535UL, 0UL, &value) != 0 ||
        out == NULL) {
        return -1;
    }
    *out = (uint16_t)value;
    return 0;
}

static int team_http_query_i32(const char *path, const char *key, int32_t min_value, int32_t max_value,
    int32_t *out)
{
    int64_t value;
    if (team_http_query_number(path, key, (int64_t)min_value, (int64_t)max_value,
        2147483647UL, 2147483648UL, &value) != 0 || out == NULL) {
        return -1;
    }
    *out = (int32_t)value;
    return 0;
}

static int team_http_query_hex16(const char *path, const char *key, uint16_t *out)
{
    const char *p;
    uint16_t value = 0U;
    uint8_t digits = 0U;

    if (out == NULL) {
        return -1;
    }
    p = team_http_query_value_start(path, key);
    if (p == NULL) {
        return -1;
    }
    while (digits < 4U) {
        char ch = *p++;
        uint8_t nibble;
        if (ch >= '0' && ch <= '9') {
            nibble = (uint8_t)(ch - '0');
        } else if (ch >= 'a' && ch <= 'f') {
            nibble = (uint8_t)(ch - 'a' + 10);
        } else if (ch >= 'A' && ch <= 'F') {
            nibble = (uint8_t)(ch - 'A' + 10);
        } else {
            return -1;
        }
        value = (uint16_t)((value << 4U) | nibble);
        digits++;
    }
    if (*p != '\0' && *p != '&') {
        return -1;
    }
    *out = value;
    return 0;
}

static void team_http_send_config_status_json(int fd)
{
    int ret = g_team_http.cb.write_config_status_json != NULL ?
        g_team_http.cb.write_config_status_json(g_team_http_json, sizeof(g_team_http_json)) :
        SLE_TEAM_ERR_UNSUPPORTED;
    team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
        ret < 0 ? "{\"ok\":false,\"error\":\"config-status\"}" : g_team_http_json);
}

static void team_http_send_config_action_json(int fd, const char *action, int action_ret)
{
    char status_json[768];
    int status_ret = g_team_http.cb.write_config_status_json != NULL ?
        g_team_http.cb.write_config_status_json(status_json, sizeof(status_json)) :
        SLE_TEAM_ERR_UNSUPPORTED;
    int len;

    if (status_ret < 0) {
        (void)snprintf(status_json, sizeof(status_json), "{\"ok\":false,\"error\":\"status\",\"ret\":%d}",
            status_ret);
    }
    len = snprintf(g_team_http_json, sizeof(g_team_http_json),
        "{\"ok\":%s,\"action\":\"%s\",\"ret\":%d,\"config\":%s}",
        action_ret == SLE_TEAM_OK ? "true" : "false",
        action != NULL ? action : "config", action_ret, status_json);
    team_http_send_response(fd, len < 0 || len >= (int)sizeof(g_team_http_json) ?
        "500 Internal Server Error" : "200 OK", "application/json",
        len < 0 || len >= (int)sizeof(g_team_http_json) ?
            "{\"ok\":false,\"error\":\"config-action\"}" : g_team_http_json);
}

static void team_http_append_str(char *buf, size_t buf_size, size_t *used, const char *text)
{
    size_t text_len;
    if (buf == NULL || used == NULL || text == NULL || *used >= buf_size) {
        return;
    }
    text_len = strlen(text);
    if (text_len >= buf_size - *used) {
        text_len = buf_size - *used - 1U;
    }
    if (text_len > 0U) {
        (void)memcpy_s(buf + *used, buf_size - *used, text, text_len);
        *used += text_len;
        buf[*used] = '\0';
    }
}

static void team_http_append_fmt(char *buf, size_t buf_size, size_t *used, const char *fmt, ...)
{
    va_list ap;
    int len;
    if (buf == NULL || used == NULL || fmt == NULL || *used >= buf_size) {
        return;
    }
    va_start(ap, fmt);
    len = vsnprintf(buf + *used, buf_size - *used, fmt, ap);
    va_end(ap);
    if (len <= 0) {
        return;
    }
    if (len >= (int)(buf_size - *used)) {
        *used = buf_size - 1U;
        buf[*used] = '\0';
        return;
    }
    *used += (size_t)len;
}

static void team_http_append_html_shell_start(char *buf, size_t buf_size, size_t *used, const char *active)
{
    static const char * const chunks[] = {
        "<!doctype html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">",
        "<title>" WS63_CONSOLE_BOARD_TITLE "</title><style>",
        "body{margin:0;font-family:-apple-system,BlinkMacSystemFont,Segoe UI,sans-serif;"
        "background:" WS63_CONSOLE_COLOR_PAGE_BG ";color:" WS63_CONSOLE_COLOR_TEXT
        "}header{padding:18px 16px;background:" WS63_CONSOLE_COLOR_HEADER_BG ";color:white}",
        "h1{font-size:22px;margin:0 0 4px}.sub{opacity:.72;font-size:13px}"
        "main{padding:14px;display:grid;gap:12px}",
        ".card{background:" WS63_CONSOLE_COLOR_CARD_BG ";border:1px solid " WS63_CONSOLE_COLOR_BORDER
        ";border-radius:8px;padding:14px}"
        ".row{display:flex;justify-content:space-between;gap:12px;padding:7px 0;border-bottom:1px solid #edf0f3}",
        ".row:last-child{border-bottom:0}.k{color:" WS63_CONSOLE_COLOR_MUTED
        "}.v{font-weight:600;text-align:right;word-break:break-word}"
        "pre{white-space:pre-wrap;word-break:break-word;margin:0;font-size:12px;line-height:1.45}",
        ".ok{color:" WS63_CONSOLE_COLOR_OK "}.bad{color:" WS63_CONSOLE_COLOR_BAD "}.warn{color:"
        WS63_CONSOLE_COLOR_WARN "}.bar{display:flex;gap:8px;margin-top:10px;flex-wrap:wrap}"
        ".tag{font-size:12px;color:" WS63_CONSOLE_COLOR_MUTED ";margin-top:8px}"
        ".stats{display:grid;grid-template-columns:repeat(3,minmax(0,1fr));gap:8px;margin-top:10px}"
        ".stat{border:1px solid #e1e6ee;border-radius:8px;padding:8px;background:#fbfcfd}"
        ".stat span{display:block;font-size:11px;color:" WS63_CONSOLE_COLOR_MUTED "}.stat strong{font-size:20px}"
        ".topology{display:grid;gap:8px;margin-top:10px}.top-row{display:flex;align-items:center;gap:8px;flex-wrap:wrap}"
        ".top-row.top-child{margin-left:18px}"
        ".top-node{display:inline-flex;align-items:center;min-height:28px;padding:0 8px;border-radius:8px;border:1px solid #d6dde6;background:#fff;font-weight:600}"
        ".top-node.leader{border-color:#f4b856;background:#fff5dc}.top-node.relay{border-color:#9e8bea;background:#f2eeff}"
        ".top-node.offline{opacity:.55}.top-edge{color:" WS63_CONSOLE_COLOR_MUTED ";font-size:12px}"
        "a,button{font:inherit;border:1px solid #c9d0da;border-radius:6px;background:white;color:#182230;padding:8px 10px;text-decoration:none}"
        "input{font:inherit;border:1px solid #c9d0da;border-radius:6px;padding:8px 10px;max-width:86px}"
        "form{display:flex;gap:8px;flex-wrap:wrap;margin-top:10px}"
        "a.on{background:#182230;color:#fff;border-color:#182230}",
        "</style></head><body><header><h1>" WS63_CONSOLE_BOARD_TITLE "</h1>"
        "<div class=\"sub\">" WS63_CONSOLE_BOARD_SUBTITLE "</div></header><main>"
    };
    size_t i;

    buf[0] = '\0';
    *used = 0U;
    for (i = 0U; i < sizeof(chunks) / sizeof(chunks[0]); i++) {
        team_http_append_str(buf, buf_size, used, chunks[i]);
        if (i == 0U && active != NULL && strcmp(active, "status") == 0) {
            team_http_append_str(buf, buf_size, used, "<meta http-equiv=\"refresh\" content=\"3\">");
        }
    }
    team_http_append_fmt(buf, buf_size, used,
        "<div class=\"bar\"><a class=\"%s\" href=\"" WS63_CONSOLE_TAB_STATUS_HREF "\">"
        WS63_CONSOLE_TAB_STATUS_LABEL "</a><a class=\"%s\" href=\"" WS63_CONSOLE_TAB_NODES_HREF "\">"
        WS63_CONSOLE_TAB_NODES_LABEL "</a><a class=\"%s\" href=\"" WS63_CONSOLE_TAB_EVENTS_HREF "\">"
        WS63_CONSOLE_TAB_EVENTS_LABEL "</a><a class=\"%s\" href=\"/pairing\">pairing</a><a href=\""
        WS63_CONSOLE_TAB_JSON_HREF "\">" WS63_CONSOLE_TAB_JSON_LABEL "</a></div>",
        strcmp(active, "status") == 0 ? "on" : "",
        strcmp(active, "nodes") == 0 ? "on" : "",
        strcmp(active, "events") == 0 ? "on" : "",
        strcmp(active, "pairing") == 0 ? "on" : "");
    team_http_append_fmt(buf, buf_size, used, "<div class=\"tag\">page=%s " WS63_CONSOLE_BOARD_VERSION "</div>",
        active != NULL ? active : "status");
}

static void team_http_append_html_end(char *buf, size_t buf_size, size_t *used)
{
    team_http_append_str(buf, buf_size, used, "</main></body></html>");
}

static void team_http_send_status_json_response(int fd)
{
    ws63_team_http_identity_t identity;
    int ret;
    size_t used;

    team_http_get_identity(&identity);
    if (g_team_http.node == NULL || identity.role_configured == 0U) {
        used = (size_t)snprintf(g_team_http_json, sizeof(g_team_http_json),
            "{\"configured\":false,\"selfLabel\":\"%s\",\"routeId\":%u,\"macReady\":%s,"
            "\"macSuffix\":\"%04X\",\"ssid\":\"%s\",\"transport\":\"ws63-softap\","
            "\"roleRequestPending\":%s,\"roleRequestRole\":\"%s\",\"roleRequestRoleValue\":%u,\"roleRequestTeam\":%u,"
            "\"roleRequestChannel\":%u,\"roleRequestLeader\":%u,\"roleRequestLeaderSuffix\":\"%04X\","
            "\"roleRequestLeaderTerm\":%u,\"roleRequestLastRet\":%d,"
            "\"onlineNodeCount\":%u,\"relayNodeCount\":%u}",
            identity.self_label, identity.route_id, identity.self_mac_ready != 0U ? "true" : "false",
            identity.self_suffix, identity.softap_ssid,
            identity.role_request_pending != 0U ? "true" : "false",
            team_http_role_name(identity.role_request_role, identity.role_request_pending),
            identity.role_request_pending != 0U ? identity.role_request_role : 255U,
            identity.role_request_pending != 0U ? identity.role_request_team : 0U,
            identity.role_request_pending != 0U ? identity.role_request_channel : 0U,
            identity.role_request_pending != 0U ? identity.role_request_leader : 0U,
            identity.role_request_pending != 0U ? identity.role_request_leader_suffix : 0U,
            identity.role_request_pending != 0U ? identity.role_request_leader_term : 0U,
            identity.role_request_last_ret,
            team_http_online_node_count(), team_http_relay_node_count());
        team_http_send_response(fd, used >= sizeof(g_team_http_json) ?
            "500 Internal Server Error" : "200 OK", "application/json",
            used >= sizeof(g_team_http_json) ? "{\"ok\":false,\"error\":\"status\"}" : g_team_http_json);
        return;
    }
    ret = sle_team_web_write_status_json(g_team_http.node, team_http_now_s(), "ws63-softap",
        g_team_http_json, sizeof(g_team_http_json));
    team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
        ret < 0 ? "{\"ok\":false,\"error\":\"status\"}" : g_team_http_json);
}

static void team_http_send_location_event(int fd, uint8_t dst_id, int send_ret, int32_t lat, int32_t lon)
{
    const uint32_t now_s = team_http_now_s();
    const uint16_t seq = g_team_http.node != NULL && g_team_http.node->next_seq != 0U ?
        (uint16_t)(g_team_http.node->next_seq - 1U) : 0U;
    int len;

    len = snprintf(g_team_http_json, sizeof(g_team_http_json),
        "{\"id\":\"ws63-location-%lu\",\"time\":\"%lu\",\"direction\":\"%s\","
        "\"type\":\"POS_REPORT\",\"srcId\":%u,\"dstId\":%u,\"seq\":%u,"
        "\"summary\":\"lat=%ld lon=%ld ret=%d\",\"ret\":%d}",
        (unsigned long)now_s, (unsigned long)now_s, send_ret == SLE_TEAM_OK ? "tx" : "system",
        g_team_http.node != NULL ? g_team_http.node->cfg.self_id : 0U, dst_id, seq,
        (long)lat, (long)lon, send_ret, send_ret);
    team_http_send_response(fd, len < 0 || len >= (int)sizeof(g_team_http_json) ?
        "500 Internal Server Error" : (send_ret == SLE_TEAM_OK ? "200 OK" : "500 Internal Server Error"),
        "application/json",
        len < 0 || len >= (int)sizeof(g_team_http_json) ?
            "{\"ok\":false,\"error\":\"location\"}" : g_team_http_json);
}

static void team_http_append_topology(char *buf, size_t buf_size, size_t *used)
{
    ws63_team_http_identity_t identity;
    uint8_t i;
    uint8_t wrote = 0U;
    char node_label[16];

    if (g_team_http.node == NULL) {
        return;
    }
    team_http_get_identity(&identity);
    team_http_append_str(buf, buf_size, used,
        "<section class=\"card\"><h2 style=\"font-size:16px;margin:0 0 10px\">Topology</h2>"
        "<div class=\"topology\">");
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"top-row\"><span class=\"top-node leader\">%s</span>"
            "<span class=\"top-edge\">online nodes=%u relays=%u</span></div>",
            identity.self_label, team_http_online_node_count(), team_http_relay_node_count());
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            const sle_team_member_record_t *member = &g_team_http.node->members[i];
            if (member->online == 0U && member->policy_pending == 0U && member->position_valid == 0U) {
                continue;
            }
            if (member->relay_allowed != 0U || member->parent_id != g_team_http.node->cfg.self_id) {
                continue;
            }
            team_http_format_route_label(member->member_id, member->role, member->mac, member->mac_ready,
                node_label, sizeof(node_label));
            team_http_append_fmt(buf, buf_size, used,
                "<div class=\"top-row\"><span class=\"top-edge\">|- direct</span>"
                "<span class=\"top-node member%s\">%s</span>"
                "<span class=\"top-edge\">next=%u children=%u</span></div>",
                member->online != 0U ? "" : " offline", node_label, member->next_hop_id, member->child_count);
            wrote = 1U;
        }
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            uint8_t j;
            uint8_t child_count = 0U;
            const sle_team_member_record_t *relay = &g_team_http.node->members[i];
            if (relay->member_id == 0U || relay->relay_allowed == 0U ||
                (relay->online == 0U && relay->policy_pending == 0U && relay->position_valid == 0U)) {
                continue;
            }
            team_http_format_route_label(relay->member_id, relay->role, relay->mac, relay->mac_ready,
                node_label, sizeof(node_label));
            team_http_append_fmt(buf, buf_size, used,
                "<div class=\"top-row\"><span class=\"top-edge\">|- relay</span>"
                "<span class=\"top-node relay%s\">%s RELAY</span>"
                "<span class=\"top-edge\">route=%u next=%u children=%u</span></div>",
                relay->online != 0U ? "" : " offline", node_label, relay->member_id,
                relay->next_hop_id, relay->child_count);
            wrote = 1U;
            for (j = 0U; j < SLE_TEAM_MAX_MEMBERS; j++) {
                const sle_team_member_record_t *child = &g_team_http.node->members[j];
                if (child->member_id == 0U || child->member_id == relay->member_id ||
                    child->parent_id != relay->member_id ||
                    (child->online == 0U && child->policy_pending == 0U && child->position_valid == 0U)) {
                    continue;
                }
                team_http_format_route_label(child->member_id, child->role, child->mac, child->mac_ready,
                    node_label, sizeof(node_label));
                team_http_append_fmt(buf, buf_size, used,
                    "<div class=\"top-row top-child\"><span class=\"top-edge\">   |-- child</span>"
                    "<span class=\"top-node member%s\">%s</span>"
                    "<span class=\"top-edge\">parent=%u next=%u</span></div>",
                    child->online != 0U ? "" : " offline", node_label, child->parent_id, child->next_hop_id);
                child_count++;
            }
            if (child_count == 0U) {
                team_http_append_str(buf, buf_size, used,
                    "<div class=\"top-row top-child\"><span class=\"top-edge\">   |-- no relay children</span></div>");
            }
        }
        if (wrote == 0U) {
            team_http_append_str(buf, buf_size, used,
                "<div class=\"top-row\"><span class=\"top-edge\">no members online</span></div>");
        }
    } else {
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"top-row\"><span class=\"top-node leader\">%s</span>"
            "<span class=\"top-edge\">parent</span><span class=\"top-node %s\">%s</span></div>",
            identity.leader_label,
            g_team_http.node->joined != 0U ? "member" : "offline",
            identity.self_label);
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"top-row\"><span class=\"top-edge\">joined=%s parent=%u relay=%s</span></div>",
            g_team_http.node->joined != 0U ? "true" : "false",
            g_team_http.node->upstream_parent_id,
            g_team_http.node->cfg.relay_enabled != 0U ? "enabled" :
                (g_team_http.node->cfg.relay_allowed != 0U ? "allowed" : "off"));
    }
    team_http_append_str(buf, buf_size, used, "</div></section>");
}

static void team_http_append_node_rows(char *buf, size_t buf_size, size_t *used,
    const sle_team_member_record_t *member)
{
    char node_label[16];

    if (member == NULL) {
        return;
    }
    team_http_format_route_label(member->member_id, member->role, member->mac, member->mac_ready,
        node_label, sizeof(node_label));
    team_http_append_fmt(buf, buf_size, used,
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_NODE_NODE_LABEL
        "</span><span class=\"v\">%s %s parent=%u next=%u children=%u</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_NODE_BATTERY_LABEL
        "</span><span class=\"v\">%u%%</span></div>",
        node_label, member->online != 0U ? "online" : "offline",
        member->parent_id, member->next_hop_id, member->child_count, member->battery_percent);
    if (member->position_valid != 0U) {
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"row\"><span class=\"k\">GPS</span>"
            "<span class=\"v\">%s fix=%u sat=%u lat=%ld lon=%ld speed=%u heading=%u</span></div>",
            member->online != 0U ? "live" : "last", member->fix_status, member->sat_count,
            (long)member->latitude_e6, (long)member->longitude_e6,
            member->speed_cms, member->heading_deg);
    } else {
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"row\"><span class=\"k\">GPS</span>"
            "<span class=\"v\">no fix fix=%u sat=%u</span></div>",
            member->fix_status, member->sat_count);
    }
    if (member->last_rssi_dbm == SLE_TEAM_RSSI_UNKNOWN) {
        team_http_append_str(buf, buf_size, used,
            "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_NODE_RSSI_LABEL
            "</span><span class=\"v\">NA</span></div>");
    } else {
        team_http_append_fmt(buf, buf_size, used,
            "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_NODE_RSSI_LABEL
            "</span><span class=\"v\">%d dBm</span></div>", member->last_rssi_dbm);
    }
    team_http_append_fmt(buf, buf_size, used,
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_NODE_SEQ_LABEL
        "</span><span class=\"v\">%u seen=%lus relay=%u pending=%u</span></div>",
        member->last_seq, (unsigned long)member->last_seen_s,
        member->relay_allowed, member->policy_pending);
}

static void team_http_make_self_member_record(sle_team_member_record_t *self)
{
    const sle_team_member_record_t *record;

    if (self == NULL || g_team_http.node == NULL) {
        return;
    }
    record = sle_team_node_find_member(g_team_http.node, g_team_http.node->cfg.self_id);
    if (record != NULL) {
        *self = *record;
    } else {
        (void)memset_s(self, sizeof(*self), 0, sizeof(*self));
        self->member_id = g_team_http.node->cfg.self_id;
        self->role = (uint8_t)g_team_http.node->cfg.role;
        self->last_rssi_dbm = SLE_TEAM_RSSI_UNKNOWN;
    }
    self->member_id = g_team_http.node->cfg.self_id;
    self->role = (uint8_t)g_team_http.node->cfg.role;
    self->online = (uint8_t)(g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER ||
        g_team_http.node->joined != 0U ? 1U : 0U);
    self->relay_allowed = g_team_http.node->cfg.relay_allowed;
    self->relay_tier = g_team_http.node->cfg.relay_tier;
    self->max_downstream = g_team_http.node->cfg.max_downstream;
    self->parent_id = g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER ?
        g_team_http.node->upstream_parent_id : 0U;
    self->next_hop_id = g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER ?
        g_team_http.node->upstream_parent_id : 0U;
    self->last_seq = g_team_http.node->next_seq;
    if (g_team_http.node->cfg.self_mac_ready != 0U) {
        (void)memcpy_s(self->mac, sizeof(self->mac), g_team_http.node->cfg.self_mac,
            sizeof(g_team_http.node->cfg.self_mac));
        self->mac_ready = 1U;
    }
}

static void team_http_handle_location(int fd, const char *path)
{
    sle_team_pos_body_t pos;
    int send_ret;
    int local_ret;
    int32_t lat = 0;
    int32_t lon = 0;
    uint16_t speed = 0U;
    uint16_t heading = 0U;
    uint8_t battery = 100U;
    uint8_t fix = 1U;
    uint8_t sat = 0U;
    uint8_t dst = SLE_TEAM_BROADCAST_ID;

    if (g_team_http.node == NULL) {
        team_http_send_response(fd, "400 Bad Request", "application/json",
            "{\"ok\":false,\"error\":\"node_not_ready\"}");
        return;
    }
    if (team_http_query_i32(path, "lat", -90000000, 90000000, &lat) != 0 ||
        team_http_query_i32(path, "lon", -180000000, 180000000, &lon) != 0) {
        team_http_send_response(fd, "400 Bad Request", "application/json",
            "{\"ok\":false,\"error\":\"lat_lon_required\"}");
        return;
    }
    (void)team_http_query_u16(path, "speed", 0U, 65535U, &speed);
    (void)team_http_query_u16(path, "heading", 0U, 65535U, &heading);
    (void)team_http_query_u8(path, "battery", 0U, 100U, &battery);
    (void)team_http_query_u8(path, "fix", 0U, 255U, &fix);
    (void)team_http_query_u8(path, "sat", 0U, 255U, &sat);
    (void)team_http_query_u8(path, "dst", 1U, 255U, &dst);
    (void)memset_s(&pos, sizeof(pos), 0, sizeof(pos));
    pos.latitude_e6 = lat;
    pos.longitude_e6 = lon;
    pos.speed_cms = speed;
    pos.heading_deg = heading;
    pos.battery_percent = battery;
    pos.fix_status = fix;
    pos.sat_count = sat;
    local_ret = sle_team_node_record_local_position(g_team_http.node, &pos);
    ws63_team_gps_set_fallback_position(&pos, (uint32_t)uapi_tcxo_get_ms());
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER && g_team_http.node->joined != 0U) {
        send_ret = sle_team_node_send_position(g_team_http.node, dst, &pos);
        if (send_ret == SLE_TEAM_OK && local_ret != SLE_TEAM_OK) {
            send_ret = local_ret;
        }
    } else if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER &&
        dst != SLE_TEAM_BROADCAST_ID && dst != g_team_http.node->cfg.self_id) {
        send_ret = sle_team_node_send_position(g_team_http.node, dst, &pos);
        if (send_ret == SLE_TEAM_OK && local_ret != SLE_TEAM_OK) {
            send_ret = local_ret;
        }
    } else if (g_team_http.node->cfg.self_id == 0U) {
        send_ret = SLE_TEAM_OK;
    } else {
        send_ret = local_ret;
    }
    if (g_team_http.events != NULL) {
        sle_team_web_event_push(g_team_http.events, team_http_now_s(),
            send_ret == SLE_TEAM_OK ? SLE_TEAM_WEB_EVENT_TX : SLE_TEAM_WEB_EVENT_SYSTEM,
            SLE_TEAM_APP_POS_REPORT, g_team_http.node->cfg.self_id, dst,
            g_team_http.node->next_seq, send_ret == SLE_TEAM_OK ? "phone location" : "phone location failed");
    }
    team_http_send_location_event(fd, dst, send_ret, lat, lon);
}

static void team_http_send_status_page(int fd)
{
    ws63_team_http_identity_t identity;
    ws63_team_gps_status_t gps;
    size_t used;

    team_http_get_identity(&identity);
    ws63_team_gps_get_status(&gps);
    team_http_append_html_shell_start(g_team_http_html, sizeof(g_team_http_html), &used, "status");
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used, "<section class=\"card\">");
    if (g_team_http.node == NULL || identity.role_configured == 0U) {
        team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"row\"><span class=\"k\">State</span><span class=\"v warn\">%s</span></div>"
            "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_SELF_LABEL
            "</span><span class=\"v\">%s</span></div>"
            "<div class=\"row\"><span class=\"k\">GPS</span>"
            "<span class=\"v\">ready=%u rx=%lu valid=%lu fix=%u sat=%u</span></div>"
            "<div class=\"row\"><span class=\"k\">GPS Detail</span>"
            "<span class=\"v\">fixSent=%lu noFix=%lu fmt=%lu unsup=%lu source=%u</span></div>"
            "<div class=\"row\"><span class=\"k\">SSID</span><span class=\"v\">%s</span></div>"
            "<div class=\"bar\"><a href=\"/pairing\">configure</a><a href=\"/api/gps\">gps json</a></div></section>",
            identity.role_request_pending != 0U ? "starting" : "unconfigured",
            identity.self_label,
            gps.ready, (unsigned long)gps.rx_bytes, (unsigned long)gps.valid_sentences,
            gps.last_fix_status, gps.last_sat_count,
            (unsigned long)gps.fix_sentences, (unsigned long)gps.no_fix_sentences,
            (unsigned long)gps.format_errors, (unsigned long)gps.unsupported_sentences, gps.source,
            identity.softap_ssid);
        team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
        team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
        return;
    }
    team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_STATE_LABEL
        "</span><span class=\"v ok\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_ROLE_LABEL
        "</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_SELF_LABEL
        "</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_LEADER_LABEL
        "</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_JOINED_LABEL
        "</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">Relay Allowed</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">Relay Enabled</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">Upstream Parent</span><span class=\"v\">%u</span></div>"
        "<div class=\"row\"><span class=\"k\">Parent State</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_SEQ_LABEL
        "</span><span class=\"v\">%u</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_UPTIME_LABEL
        "</span><span class=\"v\">%lus</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_TRANSPORT_LABEL
        "</span><span class=\"v\">ws63-softap</span></div>"
        "<div class=\"row\"><span class=\"k\">SSID</span><span class=\"v\">%s</span></div>"
        "<div class=\"row\"><span class=\"k\">GPS</span>"
        "<span class=\"v\">ready=%u rx=%lu valid=%lu fix=%u sat=%u</span></div>"
        "<div class=\"row\"><span class=\"k\">GPS Detail</span>"
        "<span class=\"v\">fixSent=%lu noFix=%lu fmt=%lu unsup=%lu source=%u</span></div>"
        "<div class=\"row\"><span class=\"k\">" WS63_CONSOLE_STATUS_LINK_LABEL
        "</span><span class=\"v ok\">ok</span></div>",
        sle_team_web_state_name((uint8_t)g_team_http.node->state),
        sle_team_web_role_name((uint8_t)g_team_http.node->cfg.role),
        identity.self_label, identity.leader_label, g_team_http.node->joined != 0U ? "true" : "false",
        g_team_http.node->cfg.relay_allowed != 0U ? "true" : "false",
        g_team_http.node->cfg.relay_enabled != 0U ? "true" : "false",
        g_team_http.node->upstream_parent_id,
        sle_team_web_parent_state_name((uint8_t)g_team_http.node->upstream_parent_state),
        g_team_http.node->next_seq, (unsigned long)team_http_now_s(), identity.softap_ssid,
        gps.ready, (unsigned long)gps.rx_bytes, (unsigned long)gps.valid_sentences,
        gps.last_fix_status, gps.last_sat_count,
        (unsigned long)gps.fix_sentences, (unsigned long)gps.no_fix_sentences,
        (unsigned long)gps.format_errors, (unsigned long)gps.unsupported_sentences, gps.source);
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"row\"><span class=\"k\">Direct Cap</span><span class=\"v\">%u</span></div>"
            "<div class=\"row\"><span class=\"k\">Pairing</span><span class=\"v %s\">%s</span></div>"
            "<div class=\"stats\"><div class=\"stat\"><span>Online Nodes</span><strong>%u</strong></div>"
            "<div class=\"stat\"><span>Relay Nodes</span><strong>%u</strong></div>"
            "<div class=\"stat\"><span>Events</span><strong>%u</strong></div></div>",
            g_team_http.node->cfg.max_downstream,
            g_team_http.node->cfg.pairing_enabled != 0U ? "ok" : "warn",
            g_team_http.node->cfg.pairing_enabled != 0U ? "open" : "closed",
            team_http_online_node_count(), team_http_relay_node_count(),
            g_team_http.events != NULL ? g_team_http.events->count : 0U);
    }
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used, "</section>");
    team_http_append_topology(g_team_http_html, sizeof(g_team_http_html), &used);
    team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
    team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
}

static void team_http_send_nodes_page(int fd)
{
    uint8_t i;
    uint8_t wrote = 0U;
    size_t used;

    team_http_append_html_shell_start(g_team_http_html, sizeof(g_team_http_html), &used, "nodes");
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
        "<section class=\"card\"><h2 style=\"font-size:16px;margin:0 0 10px\">Nodes</h2>");
    if (g_team_http.node != NULL) {
        if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER) {
            sle_team_member_record_t self;
            team_http_make_self_member_record(&self);
            team_http_append_node_rows(g_team_http_html, sizeof(g_team_http_html), &used, &self);
            wrote = 1U;
        }
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            const sle_team_member_record_t *member = &g_team_http.node->members[i];
            if (member->online == 0U && member->policy_pending == 0U && member->position_valid == 0U) {
                continue;
            }
            if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_MEMBER &&
                member->member_id == g_team_http.node->cfg.self_id) {
                continue;
            }
            wrote = 1U;
            team_http_append_node_rows(g_team_http_html, sizeof(g_team_http_html), &used, member);
        }
    }
    if (wrote == 0U) {
        team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
            "<pre>[]\n" WS63_CONSOLE_EMPTY_NODES "</pre>");
    }
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used, "</section>");
    team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
    team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
}

static void team_http_send_events_page(int fd)
{
    uint8_t i;
    size_t used;
    char src_label[16];
    char dst_label[16];
    const char *direction;

    team_http_append_html_shell_start(g_team_http_html, sizeof(g_team_http_html), &used, "events");
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
        "<section class=\"card\"><h2 style=\"font-size:16px;margin:0 0 10px\">Events</h2>");
    if (g_team_http.events == NULL || g_team_http.events->count == 0U) {
        team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
            "<pre>[]\n" WS63_CONSOLE_EMPTY_EVENTS "</pre>");
    } else {
        for (i = 0U; i < g_team_http.events->count; i++) {
            uint8_t index = (uint8_t)((g_team_http.events->head + SLE_TEAM_WEB_EVENT_COUNT - 1U - i) %
                SLE_TEAM_WEB_EVENT_COUNT);
            const sle_team_web_event_t *event = &g_team_http.events->events[index];
            team_http_format_route_label(event->src_id,
                g_team_http.node != NULL && event->src_id == g_team_http.node->cfg.leader_id ?
                    (uint8_t)SLE_TEAM_ROLE_LEADER : (uint8_t)SLE_TEAM_ROLE_MEMBER,
                NULL, 0U, src_label, sizeof(src_label));
            team_http_format_route_label(event->dst_id,
                g_team_http.node != NULL && event->dst_id == g_team_http.node->cfg.leader_id ?
                    (uint8_t)SLE_TEAM_ROLE_LEADER : (uint8_t)SLE_TEAM_ROLE_MEMBER,
                NULL, 0U, dst_label, sizeof(dst_label));
            direction = event->direction == SLE_TEAM_WEB_EVENT_RX ? "rx" :
                (event->direction == SLE_TEAM_WEB_EVENT_TX ? "tx" : "system");
            team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
                "<div class=\"row\"><span class=\"k\">%lu %s %s</span><span class=\"v\">%s-%s #%u</span></div>",
                (unsigned long)event->time_s, direction, sle_team_web_msg_type_name(event->app_msg_type),
                src_label, dst_label, event->seq);
            if (event->summary[0] != '\0') {
                team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
                    "<div class=\"row\"><span class=\"k\">summary</span><span class=\"v\">%s</span></div>",
                    event->summary);
            }
        }
    }
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used, "</section>");
    team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
    team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
}

static void team_http_send_pairing_page(int fd)
{
    ws63_team_http_identity_t identity;
    uint8_t i;
    uint8_t wrote = 0U;
    size_t used;
    char node_label[16];

    team_http_get_identity(&identity);
    team_http_append_html_shell_start(g_team_http_html, sizeof(g_team_http_html), &used, "pairing");
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
        "<section class=\"card\"><h2 style=\"font-size:16px;margin:0 0 10px\">Pairing</h2>");
    if (g_team_http.node == NULL || identity.role_configured == 0U) {
        team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"row\"><span class=\"k\">Device</span><span class=\"v\">%s</span></div>"
            "<div class=\"row\"><span class=\"k\">State</span><span class=\"v %s\">%s</span></div>"
            "<div class=\"row\"><span class=\"k\">Default team/channel</span><span class=\"v\">%u / %u</span></div>",
            identity.self_label,
            identity.role_request_pending != 0U ? "warn" : "ok",
            identity.role_request_pending != 0U ? "starting SLE" : "ready",
            team_http_default_team(), team_http_default_channel());
        team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"tag\">Choose this board role after boot. Config API saves NV; now=1 also starts SLE immediately.</div>"
            "<form action=\"/api/config/leader\" method=\"get\">"
            "<label>team</label><input name=\"team\" type=\"number\" min=\"1\" max=\"254\" value=\"1\">"
            "<label>channel</label><input name=\"channel\" type=\"number\" min=\"0\" max=\"255\" value=\"17\">"
            "<label>now</label><input name=\"now\" type=\"number\" min=\"0\" max=\"1\" value=\"1\">"
            "<button type=\"submit\">save leader</button></form>"
            "<form action=\"/api/config/member\" method=\"get\">"
            "<label>team</label><input name=\"team\" type=\"number\" min=\"1\" max=\"254\" value=\"1\">"
            "<label>leader suffix</label><input name=\"leader\" type=\"text\" maxlength=\"4\" placeholder=\"279A\" value=\"\">"
            "<label>channel</label><input name=\"channel\" type=\"number\" min=\"0\" max=\"255\" value=\"17\">"
            "<label>now</label><input name=\"now\" type=\"number\" min=\"0\" max=\"1\" value=\"1\">"
            "<button type=\"submit\">save member</button></form>"
            "<div class=\"bar\"><a href=\"/api/config/status\">config json</a>"
            "<a href=\"/api/config/apply\">apply saved</a><a href=\"/api/config/clear\">clear saved</a>"
            "<a href=\"/api/config/reboot\">reboot</a><a href=\"/api/factory-reset\">factory reset</a></div>"
            "</section>");
        team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
        team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
        return;
    }
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER) {
        team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"row\"><span class=\"k\">Window</span><span class=\"v %s\">%s</span></div>"
            "<div class=\"bar\"><a href=\"/api/pairing?action=start\">start</a>"
            "<a href=\"/api/pairing?action=stop\">cancel</a>"
            "<a href=\"/api/factory-reset\">factory reset</a></div>"
            "<div class=\"tag\">Pending members send HELLO while the window is open.</div>",
            g_team_http.node->cfg.pairing_enabled != 0U ? "ok" : "warn",
            g_team_http.node->cfg.pairing_enabled != 0U ? "open" : "closed");
        for (i = 0U; i < SLE_TEAM_MAX_MEMBERS; i++) {
            const sle_team_pending_member_t *member = &g_team_http.node->pending_members[i];
            if (member->active == 0U) {
                continue;
            }
            wrote = 1U;
            team_http_format_route_label(member->member_id, member->role, member->mac, member->mac_ready,
                node_label, sizeof(node_label));
            team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
                "<div class=\"row\"><span class=\"k\">%s</span>"
                "<span class=\"v\"><a href=\"/api/pairing?action=approve&id=%u&relay=1\">approve relay</a> "
                "<a href=\"/api/pairing?action=approve&id=%u&relay=0\">approve no-relay</a></span></div>",
                node_label, member->member_id, member->member_id);
        }
        if (wrote == 0U) {
            team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
                "<pre>[]\nNo pending member yet.</pre>");
        }
    } else {
        team_http_append_fmt(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"row\"><span class=\"k\">Team</span><span class=\"v\">%u</span></div>"
            "<div class=\"row\"><span class=\"k\">Leader</span><span class=\"v\">%s</span></div>"
            "<div class=\"row\"><span class=\"k\">Channel</span><span class=\"v\">%u</span></div>"
            "<div class=\"row\"><span class=\"k\">Joined</span><span class=\"v\">%s</span></div>",
            g_team_http.node->cfg.team_id, identity.leader_label, g_team_http.node->cfg.channel_hash,
            g_team_http.node->joined != 0U ? "true" : "false");
        team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
            "<div class=\"bar\"><a href=\"/api/member/leave\">leave</a></div>"
            "<form action=\"/api/member/select\" method=\"get\">"
            "<input name=\"team\" type=\"number\" min=\"1\" max=\"254\" value=\"1\">"
            "<input name=\"leader\" type=\"text\" maxlength=\"4\" value=\"\">"
            "<input name=\"channel\" type=\"number\" min=\"0\" max=\"255\" value=\"17\">"
            "<button type=\"submit\">select leader</button></form>"
            "<div class=\"tag\">Member sends HELLO after selecting a leader.</div>"
            "<div class=\"bar\"><a href=\"/api/factory-reset\">factory reset</a></div>");
    }
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used, "</section>");
    team_http_append_str(g_team_http_html, sizeof(g_team_http_html), &used,
        "<section class=\"card\"><h2 style=\"font-size:16px;margin:0 0 10px\">Phone Location</h2>"
        "<div class=\"tag\">Use phone gps once, or start auto mode for continuous POS_REPORT upload.</div>"
        "<form id=\"pairing-location-form\" action=\"/api/location\" method=\"get\">"
        "<label>lat_e6</label><input name=\"lat\" type=\"number\" value=\"0\">"
        "<label>lon_e6</label><input name=\"lon\" type=\"number\" value=\"0\">"
        "<label>dst</label><input name=\"dst\" type=\"number\" min=\"1\" max=\"255\" value=\"255\">"
        "<label>battery</label><input name=\"battery\" type=\"number\" min=\"0\" max=\"100\" value=\"88\">"
        "<label>fix</label><input name=\"fix\" type=\"number\" min=\"0\" max=\"255\" value=\"1\">"
        "<label>sat</label><input name=\"sat\" type=\"number\" min=\"0\" max=\"255\" value=\"0\">"
        "<label>speed(cm/s)</label><input name=\"speed\" type=\"number\" min=\"0\" max=\"65535\" value=\"0\">"
        "<label>heading</label><input name=\"heading\" type=\"number\" min=\"0\" max=\"65535\" value=\"0\">"
        "<button type=\"submit\">send location</button>"
        "<button id=\"pairing-location-usegps\" type=\"button\">use phone gps</button>"
        "<button id=\"pairing-location-auto\" type=\"button\" data-run=\"0\">start auto</button>"
        "</form><pre id=\"pairing-location-result\" style=\"margin-top:8px\">idle</pre>"
        "<script>(function(){var form=document.getElementById('pairing-location-form');"
        "var result=document.getElementById('pairing-location-result');"
        "var useGps=document.getElementById('pairing-location-usegps');"
        "var autoBtn=document.getElementById('pairing-location-auto');"
        "if(!form||!result||!useGps||!autoBtn){return;}"
        "if(!navigator.geolocation){result.textContent='geolocation not supported';return;}"
        "var lat=form.elements.namedItem('lat');var lon=form.elements.namedItem('lon');"
        "var speed=form.elements.namedItem('speed');var heading=form.elements.namedItem('heading');"
        "var sat=form.elements.namedItem('sat');var fix=form.elements.namedItem('fix');"
        "var watchId=null;var lastSendMs=0;var minGapMs=2500;"
        "function fill(c){if(!c){return;}lat.value=String(Math.round(c.latitude*1000000));"
        "lon.value=String(Math.round(c.longitude*1000000));"
        "speed.value=String((typeof c.speed==='number'&&c.speed>0)?Math.round(c.speed*100):0);"
        "heading.value=String((typeof c.heading==='number'&&c.heading>=0)?Math.round(c.heading):0);"
        "if(sat&&(!sat.value||sat.value==='0')){sat.value='0';}if(fix){fix.value='1';}}"
        "function sendNow(auto){var q=new URLSearchParams(new FormData(form)).toString();"
        "result.textContent=(auto?'auto ':'manual ')+'sending...';"
        "return fetch('/api/location?'+q,{cache:'no-store'}).then(function(r){return r.text().then(function(t){result.textContent=t;return r.ok;});})"
        ".catch(function(e){result.textContent='send error: '+e;return false;});}"
        "form.addEventListener('submit',function(ev){ev.preventDefault();sendNow(false);});"
        "useGps.addEventListener('click',function(){result.textContent='gps locating...';"
        "navigator.geolocation.getCurrentPosition(function(p){fill(p.coords);sendNow(false);},"
        "function(e){result.textContent='gps error: '+(e&&e.message?e.message:'unknown');},"
        "{enableHighAccuracy:true,timeout:12000,maximumAge:0});});"
        "autoBtn.addEventListener('click',function(){if(watchId!==null){navigator.geolocation.clearWatch(watchId);"
        "watchId=null;autoBtn.textContent='start auto';autoBtn.dataset.run='0';result.textContent='auto location stopped';return;}"
        "result.textContent='auto location starting...';watchId=navigator.geolocation.watchPosition(function(p){"
        "var n=Date.now();fill(p.coords);if(n-lastSendMs<minGapMs){return;}lastSendMs=n;sendNow(true);},"
        "function(e){result.textContent='auto gps error: '+(e&&e.message?e.message:'unknown');},"
        "{enableHighAccuracy:true,timeout:12000,maximumAge:1000});autoBtn.textContent='stop auto';autoBtn.dataset.run='1';});"
        "})();</script></section>");
    team_http_append_html_end(g_team_http_html, sizeof(g_team_http_html), &used);
    team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", g_team_http_html);
}

static void team_http_send_factory_reset_page(int fd)
{
    const char *body =
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Factory reset</title></head><body>"
        "<h3>Factory reset requested</h3><p>The board is rebooting to unconfigured mode.</p>"
        "<p>Reconnect to this board WiFi and open /pairing after reboot.</p>"
        "</body></html>";
    team_http_send_response(fd, "200 OK", "text/html; charset=utf-8", body);
}

static void team_http_send_factory_reset_failed_page(int fd)
{
    const char *body =
        "<!doctype html><html><head><meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<title>Factory reset failed</title></head><body>"
        "<h3>Factory reset failed</h3><p>Flash config was not cleared. Please retry from /pairing.</p>"
        "</body></html>";
    team_http_send_response(fd, "500 Internal Server Error", "text/html; charset=utf-8", body);
}

static void team_http_handle_client(int fd)
{
    int ret;
    char path[TEAM_HTTP_PATH_SIZE];

    ret = team_http_recv_request_line(fd);
    if (ret <= 0) {
        return;
    }
    team_http_get_path(path, sizeof(path));
    if (path[0] == '\0') {
        team_http_send_response(fd, "400 Bad Request", "text/plain", "bad request");
        return;
    }
    osal_printk("[team-wifi] http recv fd=%d len=%d path=%s\r\n", fd, ret, path);

    if (strncmp(g_team_http_req, "GET /api/status", 15) == 0) {
        team_http_send_status_json_response(fd);
    } else if (strncmp(g_team_http_req, "GET /api/gps", 12) == 0) {
        team_http_send_gps_json_response(fd);
    } else if (strncmp(g_team_http_req, "GET /api/nodes", 14) == 0) {
        ret = g_team_http.node != NULL ?
            sle_team_web_write_nodes_json(g_team_http.node, g_team_http_json, sizeof(g_team_http_json)) :
            SLE_TEAM_ERR_ARG;
        team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
            ret < 0 ? "{\"ok\":false,\"error\":\"nodes\"}" : g_team_http_json);
    } else if (strncmp(g_team_http_req, "GET /api/location", 17) == 0) {
        team_http_handle_location(fd, path);
    } else if (strncmp(g_team_http_req, "GET /api/pending", 16) == 0) {
        ret = g_team_http.node != NULL ?
            sle_team_web_write_pending_json(g_team_http.node, g_team_http_json, sizeof(g_team_http_json)) :
            SLE_TEAM_ERR_ARG;
        team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
            ret < 0 ? "{\"ok\":false,\"error\":\"pending\"}" : g_team_http_json);
    } else if (strncmp(g_team_http_req, "GET /api/config/status", 22) == 0) {
        team_http_send_config_status_json(fd);
    } else if (strncmp(g_team_http_req, "GET /api/config/leader", 22) == 0) {
        uint8_t team = team_http_default_team();
        uint8_t channel = team_http_default_channel();
        uint8_t now = 0U;
        uint16_t term = SLE_TEAM_LEADER_TERM_DEFAULT;
        (void)team_http_query_u8(path, "team", 1U, 254U, &team);
        (void)team_http_query_u8(path, "channel", 0U, 255U, &channel);
        (void)team_http_query_u8(path, "now", 0U, 1U, &now);
        (void)team_http_query_u16(path, "term", 0U, 65535U, &term);
        ret = g_team_http.cb.save_leader != NULL ?
            g_team_http.cb.save_leader(team, channel, term, now) : SLE_TEAM_ERR_UNSUPPORTED;
        team_http_send_config_action_json(fd, now != 0U ? "leader-now" : "leader", ret);
    } else if (strncmp(g_team_http_req, "GET /api/config/member", 22) == 0) {
        uint8_t team = team_http_default_team();
        uint8_t channel = team_http_default_channel();
        uint8_t now = 0U;
        uint16_t term = SLE_TEAM_LEADER_TERM_DEFAULT;
        uint16_t leader_suffix = 0U;
        if (team_http_query_hex16(path, "leader", &leader_suffix) != 0 || leader_suffix == 0U) {
            team_http_send_bad_request(fd, "leader suffix required");
            return;
        }
        (void)team_http_query_u8(path, "team", 1U, 254U, &team);
        (void)team_http_query_u8(path, "channel", 0U, 255U, &channel);
        (void)team_http_query_u8(path, "now", 0U, 1U, &now);
        (void)team_http_query_u16(path, "term", 0U, 65535U, &term);
        ret = g_team_http.cb.save_member != NULL ?
            g_team_http.cb.save_member(leader_suffix, team, channel, term, now) : SLE_TEAM_ERR_UNSUPPORTED;
        team_http_send_config_action_json(fd, now != 0U ? "member-now" : "member", ret);
    } else if (strncmp(g_team_http_req, "GET /api/config/apply", 21) == 0) {
        ret = g_team_http.cb.apply_saved != NULL ? g_team_http.cb.apply_saved() : SLE_TEAM_ERR_UNSUPPORTED;
        team_http_send_config_action_json(fd, "apply", ret);
    } else if (strncmp(g_team_http_req, "GET /api/config/clear", 21) == 0) {
        ret = g_team_http.cb.clear_config != NULL ? g_team_http.cb.clear_config() : SLE_TEAM_ERR_UNSUPPORTED;
        team_http_send_config_action_json(fd, "clear", ret);
    } else if (strncmp(g_team_http_req, "GET /api/config/reboot", 22) == 0) {
        if (g_team_http.cb.reboot != NULL) {
            g_team_http.cb.reboot("config-api");
        }
        team_http_send_config_action_json(fd, "reboot", SLE_TEAM_OK);
    } else if (strncmp(g_team_http_req, "GET /api/role", 13) == 0) {
        uint8_t team = team_http_default_team();
        uint8_t channel = team_http_default_channel();
        uint16_t leader_suffix = 0U;
        uint16_t term = SLE_TEAM_LEADER_TERM_DEFAULT;
        if (strstr(path, "role=leader") != NULL) {
            ret = g_team_http.cb.save_leader != NULL ?
                g_team_http.cb.save_leader(team, channel, term, 1U) : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] role leader ret=%d\r\n", ret);
        } else if (strstr(path, "role=member") != NULL &&
            team_http_query_hex16(path, "leader", &leader_suffix) == 0) {
            (void)team_http_query_u8(path, "team", 1U, 254U, &team);
            (void)team_http_query_u8(path, "channel", 0U, 255U, &channel);
            ret = g_team_http.cb.save_member != NULL ?
                g_team_http.cb.save_member(leader_suffix, team, channel, term, 1U) : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] role member leader_suffix=%04X ret=%d\r\n", leader_suffix, ret);
        }
        team_http_send_redirect(fd, "/pairing");
    } else if (strncmp(g_team_http_req, "GET /api/pairing", 16) == 0) {
        uint8_t id = 0U;
        uint8_t relay_allowed = 0U;
        if (strstr(path, "action=start") != NULL) {
            ret = g_team_http.cb.pairing_start != NULL ?
                g_team_http.cb.pairing_start() : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] pairing start ret=%d\r\n", ret);
        } else if (strstr(path, "action=stop") != NULL) {
            ret = g_team_http.cb.pairing_stop != NULL ?
                g_team_http.cb.pairing_stop() : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] pairing stop ret=%d\r\n", ret);
        } else if (strstr(path, "action=approve") != NULL &&
            team_http_query_u8(path, "id", 1U, 254U, &id) == 0) {
            (void)team_http_query_u8(path, "relay", 0U, 1U, &relay_allowed);
            ret = g_team_http.cb.pairing_approve != NULL ?
                g_team_http.cb.pairing_approve(id, relay_allowed) : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] pairing approve id=%u relay=%u ret=%d\r\n", id, relay_allowed, ret);
        }
        team_http_send_redirect(fd, "/pairing");
    } else if (strncmp(g_team_http_req, "GET /api/member/select", 22) == 0) {
        uint8_t team = 0U;
        uint8_t channel = 0U;
        uint16_t leader_suffix = 0U;
        if (team_http_query_u8(path, "team", 1U, 254U, &team) == 0 &&
            team_http_query_u8(path, "channel", 0U, 255U, &channel) == 0 &&
            team_http_query_hex16(path, "leader", &leader_suffix) == 0) {
            ret = g_team_http.cb.member_select != NULL ?
                g_team_http.cb.member_select(leader_suffix, team, channel) : SLE_TEAM_ERR_UNSUPPORTED;
            osal_printk("[team-wifi] member select leader=%04X team=%u channel=%u ret=%d\r\n",
                leader_suffix, team, channel, ret);
        }
        team_http_send_redirect(fd, "/pairing");
    } else if (strncmp(g_team_http_req, "GET /api/member/leave", 21) == 0) {
        ret = g_team_http.cb.member_leave != NULL ? g_team_http.cb.member_leave() : SLE_TEAM_ERR_UNSUPPORTED;
        osal_printk("[team-wifi] member leave ret=%d\r\n", ret);
        team_http_send_redirect(fd, "/pairing");
    } else if (strncmp(g_team_http_req, "GET /api/factory-reset", 22) == 0) {
        ret = g_team_http.cb.factory_reset != NULL ? g_team_http.cb.factory_reset() : SLE_TEAM_ERR_UNSUPPORTED;
        if (ret == SLE_TEAM_OK) {
            team_http_send_factory_reset_page(fd);
            if (g_team_http.cb.reboot != NULL) {
                g_team_http.cb.reboot("factory-reset");
            }
        } else {
            team_http_send_factory_reset_failed_page(fd);
        }
    } else if (strncmp(g_team_http_req, "GET /api/events", 15) == 0) {
        ret = g_team_http.events != NULL ?
            sle_team_web_write_events_json(g_team_http.events, g_team_http_json, sizeof(g_team_http_json)) :
            SLE_TEAM_ERR_ARG;
        team_http_send_response(fd, ret < 0 ? "500 Internal Server Error" : "200 OK", "application/json",
            ret < 0 ? "{\"ok\":false,\"error\":\"events\"}" : g_team_http_json);
    } else if (strncmp(g_team_http_req, "GET /nodes", 10) == 0) {
        team_http_send_nodes_page(fd);
    } else if (strncmp(g_team_http_req, "GET /events", 11) == 0) {
        team_http_send_events_page(fd);
    } else if (strncmp(g_team_http_req, "GET /pairing", 12) == 0) {
        team_http_send_pairing_page(fd);
    } else if (strncmp(g_team_http_req, "GET /favicon.ico", 16) == 0) {
        team_http_send_response(fd, "204 No Content", "text/plain", "");
    } else {
        team_http_send_status_page(fd);
    }
}

static int team_http_start_softap(void)
{
    softap_config_stru conf;
    softap_config_advance_stru adv;
    softap_config_advance_stru fallback;
    struct netif *netif_p;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    uint32_t waited_ms = 0U;
    char ifname[] = "ap0";
    ws63_team_http_identity_t identity;
    errcode_t ret;

    if (wifi_is_wifi_inited() == 0) {
        ret = wifi_init();
        osal_printk("[team-wifi] wifi_init ret=0x%x\r\n", ret);
    }
    while (wifi_is_wifi_inited() == 0 && waited_ms < TEAM_HTTP_WIFI_WAIT_MS) {
        osal_msleep(100);
        waited_ms += 100U;
    }
    if (wifi_is_wifi_inited() == 0) {
        osal_printk("[team-wifi] wifi init timeout\r\n");
        return -1;
    }
    if (wifi_is_softap_enabled() != 0) {
        return 0;
    }
    team_http_get_identity(&identity);
    if (identity.softap_ssid[0] != '\0') {
        (void)snprintf(g_team_http.ssid, sizeof(g_team_http.ssid), "%s", identity.softap_ssid);
    }
    (void)memset_s(&conf, sizeof(conf), 0, sizeof(conf));
    (void)memset_s(&adv, sizeof(adv), 0, sizeof(adv));
    (void)snprintf((char *)conf.ssid, sizeof(conf.ssid), "%s", g_team_http.ssid);
    (void)snprintf((char *)conf.pre_shared_key, sizeof(conf.pre_shared_key), "%s", CONFIG_SLE_TEAM_WIFI_AP_PSK);
    conf.security_type = TEAM_HTTP_WIFI_SECURITY_PRIMARY;
    conf.channel_num = CONFIG_SLE_TEAM_WIFI_AP_CHANNEL;
    conf.wifi_psk_type = WIFI_WPA_PSK_NOT_USE;
    adv.beacon_interval = 100U;
    adv.dtim_period = 2U;
    adv.group_rekey = 86400U;
    adv.protocol_mode = TEAM_HTTP_WIFI_PROTOCOL_PRIMARY;
    /* iOS discovery is more reliable with a plain WPA2 + 11b/g/n SoftAP. Keep
     * the broader mix/AX mode as a fallback for SDK/board combinations that
     * reject the conservative primary config. */
    adv.hidden_ssid_flag = 1U;
    ret = wifi_set_softap_config_advance(&adv);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[team-wifi] softap advance config failed ret=0x%x\r\n", ret);
        return -1;
    }
    ret = wifi_softap_enable(&conf);
    if (ret != ERRCODE_SUCC) {
        osal_printk("[team-wifi] softap enable failed ret=0x%x with wpa2+11bgn, fallback to mix/ax\r\n", ret);
        conf.security_type = TEAM_HTTP_WIFI_SECURITY_COMPAT_MIX;
        fallback = adv;
        fallback.protocol_mode = TEAM_HTTP_WIFI_PROTOCOL_COMPAT_AX;
        ret = wifi_set_softap_config_advance(&fallback);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[team-wifi] softap fallback advance config failed ret=0x%x\r\n", ret);
            return -1;
        }
        ret = wifi_softap_enable(&conf);
        if (ret != ERRCODE_SUCC) {
            osal_printk("[team-wifi] softap fallback enable failed ret=0x%x\r\n", ret);
            return -1;
        }
    }
    IP4_ADDR(&ipaddr, 192, 168, 43, CONFIG_SLE_TEAM_WIFI_AP_IP_LAST);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 43, CONFIG_SLE_TEAM_WIFI_AP_IP_LAST);
    netif_p = netif_find(ifname);
    if (netif_p == NULL ||
        netifapi_netif_set_addr(netif_p, &ipaddr, &netmask, &gw) != 0 ||
        netifapi_dhcps_start(netif_p, NULL, 0) != 0) {
        osal_printk("[team-wifi] softap netif/dhcp failed, keeping AP up for static IP\r\n");
    }
    osal_printk("[team-wifi] softap started ssid=%s ip=192.168.43.%u channel=%u\r\n",
        g_team_http.ssid, CONFIG_SLE_TEAM_WIFI_AP_IP_LAST, CONFIG_SLE_TEAM_WIFI_AP_CHANNEL);
    return 0;
}

static void team_http_server_loop(void)
{
    int listen_fd;
    int client_fd;
    int opt = 1;
    struct sockaddr_in addr;

    while (1) {
        listen_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (listen_fd < 0) {
            g_team_http.last_errno = errno;
            osal_printk("[team-wifi] http socket failed errno=%d, retrying\r\n", g_team_http.last_errno);
            osal_msleep(TEAM_HTTP_RETRY_MS);
            continue;
        }
        g_team_http.listen_fd = listen_fd;
        (void)setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
        (void)memset_s(&addr, sizeof(addr), 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        addr.sin_port = htons(TEAM_HTTP_PORT);
        if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
            g_team_http.last_errno = errno;
            osal_printk("[team-wifi] http bind failed errno=%d, retrying\r\n", g_team_http.last_errno);
            lwip_close(listen_fd);
            g_team_http.listen_fd = -1;
            osal_msleep(TEAM_HTTP_RETRY_MS);
            continue;
        }
        if (listen(listen_fd, TEAM_HTTP_BACKLOG) != 0) {
            g_team_http.last_errno = errno;
            osal_printk("[team-wifi] http listen failed errno=%d, retrying\r\n", g_team_http.last_errno);
            lwip_close(listen_fd);
            g_team_http.listen_fd = -1;
            osal_msleep(TEAM_HTTP_RETRY_MS);
            continue;
        }
        break;
    }
    g_team_http.ready = 1U;
    osal_printk("[team-wifi] http server ready port=%u\r\n", TEAM_HTTP_PORT);
    while (1) {
        client_fd = accept(listen_fd, NULL, NULL);
        if (client_fd < 0) {
            osal_msleep(100);
            continue;
        }
        g_team_http.accept_count++;
        team_http_set_client_timeouts(client_fd);
        team_http_handle_client(client_fd);
        osal_msleep(20);
        (void)shutdown(client_fd, SHUT_RDWR);
        lwip_close(client_fd);
    }
}

static void *team_http_task(const char *arg)
{
    unused(arg);
    osal_printk("[team-wifi] task started\r\n");
    if (team_http_start_softap() != 0) {
        return NULL;
    }
    osal_msleep(TEAM_HTTP_START_DELAY_MS);
    team_http_server_loop();
    return NULL;
}

void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events,
    const ws63_team_http_callbacks_t *callbacks, const char *ssid)
{
    osal_task *task = NULL;

    if (g_team_http.started != 0U || node == NULL) {
        return;
    }
    g_team_http.node = node;
    g_team_http.events = events;
    g_team_http.listen_fd = -1;
    if (callbacks != NULL) {
        g_team_http.cb = *callbacks;
    } else {
        (void)memset_s(&g_team_http.cb, sizeof(g_team_http.cb), 0, sizeof(g_team_http.cb));
    }
    (void)snprintf(g_team_http.ssid, sizeof(g_team_http.ssid), "%s", ssid != NULL ? ssid : CONFIG_SLE_TEAM_WIFI_AP_SSID);
    g_team_http.started = 1U;
    osal_kthread_lock();
    task = osal_kthread_create((osal_kthread_handler)team_http_task, NULL, "TeamHttpTask",
        TEAM_HTTP_TASK_STACK_SIZE);
    if (task != NULL) {
        osal_kthread_set_priority(task, TEAM_HTTP_TASK_PRIO);
    }
    osal_kthread_unlock();
}

#else
void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events,
    const ws63_team_http_callbacks_t *callbacks, const char *ssid)
{
    unused(node);
    unused(events);
    unused(callbacks);
    unused(ssid);
}
#endif
