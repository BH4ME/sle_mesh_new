#ifndef WS63_WS2812_H
#define WS63_WS2812_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Convert logical RGB into the byte order emitted on the LED data wire. */
void ws63_ws2812_encode_frame(uint8_t red, uint8_t green, uint8_t blue, uint8_t out[3]);

#ifndef WS63_WS2812_HOST_TEST
/* Low-level GPIO/bitstream driver used by ws63_team_status_led.c. */
int ws63_ws2812_init(uint8_t pin);
int ws63_ws2812_set_rgb(uint8_t red, uint8_t green, uint8_t blue);
int ws63_ws2812_clear(void);
int ws63_ws2812_hold_low(void);

/* Diagnostics for startup/status logs and tests. */
uint8_t ws63_ws2812_is_ready(void);
uint8_t ws63_ws2812_pin(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
