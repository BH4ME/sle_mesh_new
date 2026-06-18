#ifndef SLE_TEAM_RELAY_OPTIMIZER_H
#define SLE_TEAM_RELAY_OPTIMIZER_H

#include "sle_team_node.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Stable-state relay reselection.
 *
 * This is not leader election. The configured leader may swap a weak,
 * childless, leader-direct relay for a stronger leader-direct member after the
 * group is quiet. Relay recovery, pending policy, missing allowed members, and
 * child-bearing relays are deliberately skipped.
 */
int sle_team_relay_optimizer_run(sle_team_node_t *node, uint32_t now_s);
/* Rate-limited wrapper used by the WS63 leader tick. */
uint8_t sle_team_relay_optimizer_tick(sle_team_node_t *node, uint32_t now_ms, uint32_t interval_ms,
    uint32_t *last_run_ms);

#ifdef __cplusplus
}
#endif

#endif
