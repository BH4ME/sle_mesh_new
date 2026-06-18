#ifndef WS63_TEAM_STATUS_LED_H
#define WS63_TEAM_STATUS_LED_H

#include <stdint.h>

/*
 * High-level WS2812 status policy for the team firmware.
 *
 * The network task calls one of the state setters as its role changes, then
 * calls tick() periodically. The implementation keeps one status/color active
 * at a time and applies the v4.5 low-brightness breathing curves.
 */
void ws63_team_status_led_init(void);
void ws63_team_status_led_idle(void);
void ws63_team_status_led_leader(void);
void ws63_team_status_led_member(void);
void ws63_team_status_led_relay(void);
void ws63_team_status_led_child(void);
void ws63_team_status_led_joining(void);
void ws63_team_status_led_lost(void);
void ws63_team_status_led_off(void);
void ws63_team_status_led_hold_low(uint8_t enable);
uint8_t ws63_team_status_led_hold_low_active(void);
void ws63_team_status_led_tick(uint32_t now_ms);

#endif
