#ifndef WS63_TEAM_POWER_H
#define WS63_TEAM_POWER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void ws63_team_power_init(void);
void ws63_team_power_tick(uint8_t force_now);
uint8_t ws63_team_power_battery_percent(void);
int ws63_team_power_cli_handle(const char *line);

#ifdef __cplusplus
}
#endif

#endif
