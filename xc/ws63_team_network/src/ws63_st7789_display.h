#ifndef WS63_ST7789_DISPLAY_H
#define WS63_ST7789_DISPLAY_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Board wiring and logical drawing area for the 1.14 inch ST7789 panel. */
typedef struct {
    uint8_t spi_bus;
    uint8_t sclk_pin;
    uint8_t mosi_pin;
    uint8_t cs_pin;
    uint8_t dc_pin;
    uint8_t reset_pin;
    uint16_t x_offset;
    uint16_t y_offset;
    uint16_t width;
    uint16_t height;
} ws63_st7789_config_t;

/* Display event classes published by TeamNetworkTask to TeamDisplayTask. */
typedef enum {
    WS63_ST7789_EVENT_NONE = 0,
    WS63_ST7789_EVENT_JOIN,
    WS63_ST7789_EVENT_LEFT,
    WS63_ST7789_EVENT_TIMEOUT,
    WS63_ST7789_EVENT_LOST,
    WS63_ST7789_EVENT_REJOIN,
} ws63_st7789_event_t;

/* Initialize pins, panel controller, optional LVGL backend, and boot screen. */
int ws63_st7789_init(const ws63_st7789_config_t *cfg);

/* Update the compact top status region: role, route/self label, counts, fw. */
int ws63_st7789_show_status(const char *role, const char *self, uint8_t online_count,
    uint8_t offline_count, uint8_t event_count, const char *fw_version);

/* Update the event region with member label, optional last-known GPS, and time. */
int ws63_st7789_show_event(uint8_t event, const char *member_label, int32_t latitude_e6, int32_t longitude_e6,
    uint32_t last_seen_s);

/* Advance LVGL timers/handlers; call only from the display task. */
void ws63_st7789_tick(void);

#ifdef __cplusplus
}
#endif

#endif
