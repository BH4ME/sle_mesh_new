#ifndef WS63_TEAM_STATUS_LED_H
#define WS63_TEAM_STATUS_LED_H

#include <stdint.h>

void ws63_team_status_led_init(void);
void ws63_team_status_led_idle(void);
void ws63_team_status_led_leader(void);
void ws63_team_status_led_member(void);
void ws63_team_status_led_relay(void);
void ws63_team_status_led_child(void);
void ws63_team_status_led_joining(void);
void ws63_team_status_led_lost(void);
void ws63_team_status_led_off(void);
void ws63_team_status_led_tick(uint32_t now_ms);

#endif
