#include "ws63_team_status_led.h"

#include <stdbool.h>

#include "soc_osal.h"

#include "ws63_ws2812.h"

#ifndef CONFIG_SLE_TEAM_WS2812_ENABLE
#define CONFIG_SLE_TEAM_WS2812_ENABLE 1
#endif

#ifndef CONFIG_SLE_TEAM_WS2812_PIN
#define CONFIG_SLE_TEAM_WS2812_PIN 0
#endif

#define TEAM_LED_MODE_IDLE 0U
#define TEAM_LED_MODE_LEADER 1U
#define TEAM_LED_MODE_MEMBER 2U
#define TEAM_LED_MODE_RELAY 3U
#define TEAM_LED_MODE_CHILD 4U
#define TEAM_LED_MODE_JOINING 5U
#define TEAM_LED_MODE_LOST 6U
#define TEAM_LED_PHASE_COUNT 8U
#define TEAM_LED_TICK_MS 120U
#define TEAM_LED_IDLE_MAX_SCALE 8U
#define TEAM_LED_NORMAL_MAX_SCALE 12U
#define TEAM_LED_ALERT_MAX_SCALE 20U

static const uint8_t g_status_led_breathe_idle[TEAM_LED_PHASE_COUNT] = {0U, 2U, 4U, 6U, 8U, 6U, 4U, 2U};
static const uint8_t g_status_led_breathe_normal[TEAM_LED_PHASE_COUNT] = {2U, 4U, 6U, 10U, 12U, 10U, 6U, 4U};
static const uint8_t g_status_led_breathe_alert[TEAM_LED_PHASE_COUNT] = {4U, 8U, 12U, 16U, 20U, 16U, 12U, 8U};

typedef struct {
    uint8_t ready;
    uint8_t mode;
    uint8_t applied;
    uint32_t last_tick_ms;
    uint8_t phase_index;
} ws63_team_status_led_t;

static ws63_team_status_led_t g_status_led;

static void status_led_apply(uint8_t red, uint8_t green, uint8_t blue)
{
#if CONFIG_SLE_TEAM_WS2812_ENABLE
    if (ws63_ws2812_is_ready() != 0U) {
        (void)ws63_ws2812_set_rgb(red, green, blue);
    }
#else
    (void)red;
    (void)green;
    (void)blue;
#endif
}

static uint8_t status_led_scale_u8(uint8_t value, uint8_t scale)
{
    return (uint8_t)(((uint16_t)value * (uint16_t)scale + 127U) / 255U);
}

static void status_led_apply_scaled(uint8_t red, uint8_t green, uint8_t blue, uint8_t scale)
{
    status_led_apply(status_led_scale_u8(red, scale), status_led_scale_u8(green, scale),
        status_led_scale_u8(blue, scale));
}

static void status_led_show_mode(uint8_t mode, uint8_t phase)
{
    uint8_t scale;

    if (phase >= TEAM_LED_PHASE_COUNT) {
        phase = 0U;
    }
    switch (mode) {
        case TEAM_LED_MODE_LEADER:
            scale = g_status_led_breathe_normal[phase];
            status_led_apply_scaled(0U, 64U, 255U, scale);
            break;
        case TEAM_LED_MODE_RELAY:
            scale = g_status_led_breathe_normal[phase];
            status_led_apply_scaled(160U, 0U, 255U, scale);
            break;
        case TEAM_LED_MODE_CHILD:
        case TEAM_LED_MODE_MEMBER:
            scale = g_status_led_breathe_normal[phase];
            status_led_apply_scaled(0U, 255U, 32U, scale);
            break;
        case TEAM_LED_MODE_JOINING:
            scale = g_status_led_breathe_alert[phase];
            status_led_apply_scaled(255U, 160U, 0U, scale);
            break;
        case TEAM_LED_MODE_LOST:
            scale = g_status_led_breathe_alert[phase];
            status_led_apply_scaled(255U, 0U, 0U, scale);
            break;
        case TEAM_LED_MODE_IDLE:
        default:
            scale = g_status_led_breathe_idle[phase];
            status_led_apply_scaled(255U, 255U, 255U, scale);
            break;
    }
}

static void status_led_set_mode(uint8_t mode, uint8_t immediate)
{
    if (g_status_led.applied != 0U && g_status_led.mode == mode) {
        return;
    }
    g_status_led.mode = mode;
    g_status_led.applied = 1U;
    g_status_led.phase_index = 0U;
    if (immediate != 0U) {
        status_led_show_mode(mode, g_status_led.phase_index);
    }
}

void ws63_team_status_led_init(void)
{
#if CONFIG_SLE_TEAM_WS2812_ENABLE
    if (g_status_led.ready == 0U) {
        if (ws63_ws2812_init((uint8_t)CONFIG_SLE_TEAM_WS2812_PIN) == 0) {
            g_status_led.ready = 1U;
        }
    }
    if (g_status_led.ready != 0U) {
        status_led_set_mode(TEAM_LED_MODE_IDLE, 1U);
    }
#endif
}

void ws63_team_status_led_idle(void)
{
    status_led_set_mode(TEAM_LED_MODE_IDLE, 1U);
}

void ws63_team_status_led_leader(void)
{
    status_led_set_mode(TEAM_LED_MODE_LEADER, 1U);
}

void ws63_team_status_led_member(void)
{
    status_led_set_mode(TEAM_LED_MODE_MEMBER, 1U);
}

void ws63_team_status_led_relay(void)
{
    status_led_set_mode(TEAM_LED_MODE_RELAY, 1U);
}

void ws63_team_status_led_child(void)
{
    status_led_set_mode(TEAM_LED_MODE_CHILD, 1U);
}

void ws63_team_status_led_joining(void)
{
    status_led_set_mode(TEAM_LED_MODE_JOINING, 1U);
}

void ws63_team_status_led_lost(void)
{
    status_led_set_mode(TEAM_LED_MODE_LOST, 1U);
}

void ws63_team_status_led_off(void)
{
    status_led_apply(0U, 0U, 0U);
}

void ws63_team_status_led_tick(uint32_t now_ms)
{
#if CONFIG_SLE_TEAM_WS2812_ENABLE
    if (g_status_led.ready == 0U) {
        return;
    }
    if ((uint32_t)(now_ms - g_status_led.last_tick_ms) < TEAM_LED_TICK_MS) {
        return;
    }
    g_status_led.last_tick_ms = now_ms;
    g_status_led.phase_index = (uint8_t)((g_status_led.phase_index + 1U) % TEAM_LED_PHASE_COUNT);
    status_led_show_mode(g_status_led.mode, g_status_led.phase_index);
#else
    (void)now_ms;
#endif
}
