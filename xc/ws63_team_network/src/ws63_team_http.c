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
#include "wifi_hotspot.h"
#include "wifi_hotspot_config.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifndef CONFIG_SLE_TEAM_WIFI_AP_SSID
#define CONFIG_SLE_TEAM_WIFI_AP_SSID "SLE-TEAM-V4"
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
#define TEAM_HTTP_REQ_SIZE 512
#define TEAM_HTTP_JSON_SIZE 1536
#define TEAM_HTTP_WIFI_WAIT_MS 4000U

typedef struct {
    sle_team_node_t *node;
    sle_team_web_event_log_t *events;
    const char *ssid;
    uint8_t started;
} team_http_ctx_t;

static team_http_ctx_t g_team_http;

static uint32_t team_http_now_s(void)
{
    return (uint32_t)(uapi_tcxo_get_ms() / 1000U);
}

static int team_http_digit(char c)
{
    return c >= '0' && c <= '9';
}

static const char *team_http_query_value(const char *query, const char *key)
{
    size_t key_len;
    const char *p;

    if (query == NULL || key == NULL) {
        return NULL;
    }
    key_len = strlen(key);
    p = query;
    while (*p != '\0') {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            return p + key_len + 1U;
        }
        p = strchr(p, '&');
        if (p == NULL) {
            return NULL;
        }
        p++;
    }
    return NULL;
}

static int team_http_parse_i32(const char *query, const char *key, int32_t min, int32_t max, int32_t *out)
{
    const char *p = team_http_query_value(query, key);
    int sign = 1;
    int32_t value = 0;
    uint8_t digits = 0U;

    if (p == NULL || out == NULL) {
        return -1;
    }
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (team_http_digit(*p) != 0) {
        int32_t next = (int32_t)(value * 10 + (*p - '0'));
        if (next < value) {
            return -1;
        }
        value = next;
        digits = 1U;
        p++;
    }
    if (digits == 0U || (*p != '\0' && *p != '&' && *p != ' ')) {
        return -1;
    }
    value = (int32_t)(value * sign);
    if (value < min || value > max) {
        return -1;
    }
    *out = value;
    return 0;
}

static int team_http_parse_u32(const char *query, const char *key, uint32_t max, uint32_t *out)
{
    int32_t value = 0;
    if (team_http_parse_i32(query, key, 0, (int32_t)max, &value) != 0 || out == NULL) {
        return -1;
    }
    *out = (uint32_t)value;
    return 0;
}

static int team_http_parse_coord_e6(const char *query, const char *key, int32_t min_e6, int32_t max_e6, int32_t *out)
{
    const char *p = team_http_query_value(query, key);
    int sign = 1;
    int32_t whole = 0;
    int32_t frac = 0;
    uint32_t frac_scale = 1U;
    uint8_t digits = 0U;

    if (p == NULL || out == NULL) {
        return -1;
    }
    if (*p == '-') {
        sign = -1;
        p++;
    }
    while (team_http_digit(*p) != 0) {
        whole = (int32_t)(whole * 10 + (*p - '0'));
        digits = 1U;
        p++;
    }
    if (*p == '.') {
        p++;
        while (team_http_digit(*p) != 0 && frac_scale < 1000000U) {
            frac = (int32_t)(frac * 10 + (*p - '0'));
            frac_scale *= 10U;
            digits = 1U;
            p++;
        }
        while (team_http_digit(*p) != 0) {
            p++;
        }
    }
    if (digits == 0U || (*p != '\0' && *p != '&' && *p != ' ')) {
        return -1;
    }
    while (frac_scale < 1000000U) {
        frac *= 10;
        frac_scale *= 10U;
    }
    *out = (int32_t)((whole * 1000000 + frac) * sign);
    if (*out < min_e6 || *out > max_e6) {
        return -1;
    }
    return 0;
}

static void team_http_send(int fd, const char *status, const char *type, const char *body)
{
    char header[160];
    int len;
    size_t body_len = body != NULL ? strlen(body) : 0U;

    len = snprintf(header, sizeof(header),
        "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\nConnection: close\r\n\r\n",
        status, type, (unsigned long)body_len);
    if (len > 0) {
        (void)send(fd, header, (size_t)len, 0);
    }
    if (body_len != 0U) {
        (void)send(fd, body, body_len, 0);
    }
}

static void team_http_send_json(int fd, const char *body)
{
    team_http_send(fd, "200 OK", "application/json", body != NULL ? body : "{}");
}

static void team_http_send_not_found(int fd)
{
    team_http_send(fd, "404 Not Found", "application/json", "{\"ok\":false,\"error\":\"not_found\"}");
}

static void team_http_handle_location(int fd, const char *query)
{
    sle_team_pos_body_t pos;
    int32_t lat = 0;
    int32_t lon = 0;
    uint32_t dst = SLE_TEAM_BROADCAST_ID;
    uint32_t speed = 0;
    uint32_t heading = 0;
    uint32_t battery = 100;
    uint32_t fix = 1;
    uint32_t sat = 0;
    int ret;
    char body[160];

    if (g_team_http.node == NULL ||
        team_http_parse_coord_e6(query, "lat", -90000000, 90000000, &lat) != 0 ||
        team_http_parse_coord_e6(query, "lon", -180000000, 180000000, &lon) != 0) {
        team_http_send(fd, "400 Bad Request", "application/json", "{\"ok\":false,\"error\":\"bad_location\"}");
        return;
    }
    (void)team_http_parse_u32(query, "dst", 255U, &dst);
    (void)team_http_parse_u32(query, "speed", 65535U, &speed);
    (void)team_http_parse_u32(query, "heading", 65535U, &heading);
    (void)team_http_parse_u32(query, "battery", 100U, &battery);
    (void)team_http_parse_u32(query, "fix", 255U, &fix);
    (void)team_http_parse_u32(query, "sat", 255U, &sat);
    (void)memset_s(&pos, sizeof(pos), 0, sizeof(pos));
    pos.latitude_e6 = lat;
    pos.longitude_e6 = lon;
    pos.speed_cms = (uint16_t)speed;
    pos.heading_deg = (uint16_t)heading;
    pos.battery_percent = (uint8_t)battery;
    pos.fix_status = (uint8_t)fix;
    pos.sat_count = (uint8_t)sat;
    if (g_team_http.node->cfg.role == SLE_TEAM_ROLE_LEADER &&
        (dst == SLE_TEAM_BROADCAST_ID || dst == g_team_http.node->cfg.self_id)) {
        ret = sle_team_node_record_local_position(g_team_http.node, &pos);
    } else {
        ret = sle_team_node_send_position(g_team_http.node, (uint8_t)dst, &pos);
    }
    if (g_team_http.events != NULL) {
        sle_team_web_event_push(g_team_http.events, team_http_now_s(), SLE_TEAM_WEB_EVENT_TX,
            SLE_TEAM_APP_POS_REPORT, g_team_http.node->cfg.self_id, (uint8_t)dst, g_team_http.node->next_seq,
            "phone location");
    }
    (void)snprintf(body, sizeof(body),
        "{\"ok\":%s,\"ret\":%d,\"dst\":%lu,\"latitudeE6\":%ld,\"longitudeE6\":%ld}",
        ret == SLE_TEAM_OK ? "true" : "false", ret, (unsigned long)dst, (long)lat, (long)lon);
    team_http_send_json(fd, body);
}

static void team_http_handle_request(int fd, char *req, char *json, size_t json_len)
{
    char method[8];
    char target[160];
    char *query;
    int ret;

    if (sscanf(req, "%7s %159s", method, target) != 2 || strcmp(method, "GET") != 0) {
        team_http_send(fd, "405 Method Not Allowed", "application/json", "{\"ok\":false,\"error\":\"method\"}");
        return;
    }
    query = strchr(target, '?');
    if (query != NULL) {
        *query = '\0';
        query++;
    }
    if (strcmp(target, "/api/location") == 0) {
        team_http_handle_location(fd, query);
        return;
    }
    if (g_team_http.node == NULL) {
        team_http_send(fd, "503 Service Unavailable", "application/json", "{\"ok\":false,\"error\":\"not_ready\"}");
        return;
    }
    if (strcmp(target, "/api/status") == 0 || strcmp(target, "/") == 0) {
        ret = sle_team_web_write_status_json(g_team_http.node, team_http_now_s(), "ws63-softap", json, json_len);
    } else if (strcmp(target, "/api/nodes") == 0) {
        ret = sle_team_web_write_nodes_json(g_team_http.node, json, json_len);
    } else if (strcmp(target, "/api/pending") == 0) {
        ret = sle_team_web_write_pending_json(g_team_http.node, json, json_len);
    } else if (strcmp(target, "/api/events") == 0) {
        ret = sle_team_web_write_events_json(g_team_http.events, json, json_len);
    } else {
        team_http_send_not_found(fd);
        return;
    }
    if (ret < 0) {
        team_http_send(fd, "500 Internal Server Error", "application/json", "{\"ok\":false,\"error\":\"json\"}");
        return;
    }
    team_http_send_json(fd, json);
}

static int team_http_start_softap(void)
{
    softap_config_stru conf;
    softap_config_advance_stru adv;
    struct netif *netif_p;
    ip4_addr_t ipaddr;
    ip4_addr_t netmask;
    ip4_addr_t gw;
    uint32_t waited_ms = 0U;
    char ifname[] = "ap0";

    if (wifi_is_wifi_inited() == 0) {
        (void)wifi_init();
    }
    while (wifi_is_wifi_inited() == 0 && waited_ms < TEAM_HTTP_WIFI_WAIT_MS) {
        osal_msleep(100);
        waited_ms += 100U;
    }
    if (wifi_is_wifi_inited() == 0) {
        osal_printk("[team-http] wifi init timeout\r\n");
        return -1;
    }
    if (wifi_is_softap_enabled() != 0) {
        return 0;
    }
    (void)memset_s(&conf, sizeof(conf), 0, sizeof(conf));
    (void)memset_s(&adv, sizeof(adv), 0, sizeof(adv));
    (void)snprintf((char *)conf.ssid, sizeof(conf.ssid), "%s", CONFIG_SLE_TEAM_WIFI_AP_SSID);
    (void)snprintf((char *)conf.pre_shared_key, sizeof(conf.pre_shared_key), "%s", CONFIG_SLE_TEAM_WIFI_AP_PSK);
    conf.security_type = 3;
    conf.channel_num = CONFIG_SLE_TEAM_WIFI_AP_CHANNEL;
    conf.wifi_psk_type = 0;
    adv.beacon_interval = 100U;
    adv.dtim_period = 2U;
    adv.group_rekey = 86400U;
    adv.protocol_mode = 4;
    adv.hidden_ssid_flag = 1U;
    if (wifi_set_softap_config_advance(&adv) != ERRCODE_SUCC || wifi_softap_enable(&conf) != ERRCODE_SUCC) {
        osal_printk("[team-http] softap start failed\r\n");
        return -1;
    }
    IP4_ADDR(&ipaddr, 192, 168, 43, CONFIG_SLE_TEAM_WIFI_AP_IP_LAST);
    IP4_ADDR(&netmask, 255, 255, 255, 0);
    IP4_ADDR(&gw, 192, 168, 43, 2);
    netif_p = netif_find(ifname);
    if (netif_p == NULL ||
        netifapi_netif_set_addr(netif_p, &ipaddr, &netmask, &gw) != 0 ||
        netifapi_dhcps_start(netif_p, NULL, 0) != 0) {
        osal_printk("[team-http] softap netif failed\r\n");
        (void)wifi_softap_disable();
        return -1;
    }
    osal_printk("[team-http] softap ssid=%s ip=192.168.43.%u\r\n",
        CONFIG_SLE_TEAM_WIFI_AP_SSID, CONFIG_SLE_TEAM_WIFI_AP_IP_LAST);
    return 0;
}

static void *team_http_task(const char *arg)
{
    int sock_fd;
    char req[TEAM_HTTP_REQ_SIZE];
    char json[TEAM_HTTP_JSON_SIZE];
    struct sockaddr_in addr;

    unused(arg);
    if (team_http_start_softap() != 0) {
        return NULL;
    }
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd < 0) {
        osal_printk("[team-http] socket failed\r\n");
        return NULL;
    }
    (void)memset_s(&addr, sizeof(addr), 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(TEAM_HTTP_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(sock_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(sock_fd, 2) != 0) {
        osal_printk("[team-http] bind/listen failed\r\n");
        lwip_close(sock_fd);
        return NULL;
    }
    osal_printk("[team-http] listen port=%u\r\n", TEAM_HTTP_PORT);
    while (1) {
        int client_fd = accept(sock_fd, NULL, NULL);
        int got;
        if (client_fd < 0) {
            osal_msleep(50);
            continue;
        }
        (void)memset_s(req, sizeof(req), 0, sizeof(req));
        got = recv(client_fd, req, sizeof(req) - 1U, 0);
        if (got > 0) {
            req[got] = '\0';
            team_http_handle_request(client_fd, req, json, sizeof(json));
        }
        lwip_close(client_fd);
    }
    return NULL;
}

void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events, const char *ssid)
{
    osal_task *task = NULL;

    if (g_team_http.started != 0U || node == NULL) {
        return;
    }
    g_team_http.node = node;
    g_team_http.events = events;
    g_team_http.ssid = ssid;
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
void ws63_team_http_start(sle_team_node_t *node, sle_team_web_event_log_t *events, const char *ssid)
{
    unused(node);
    unused(events);
    unused(ssid);
}
#endif
