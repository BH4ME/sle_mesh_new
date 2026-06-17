#ifndef SLE_TEAM_CLI_H
#define SLE_TEAM_CLI_H

#include "sle_team_node.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sle_team_cli_print_fn)(void *user_ctx, const char *text);

typedef struct {
    sle_team_node_t *node;
    sle_team_cli_print_fn print;
    void *user_ctx;
} sle_team_cli_t;

void sle_team_cli_init(sle_team_cli_t *cli, sle_team_node_t *node, sle_team_cli_print_fn print, void *user_ctx);
void sle_team_cli_handle_line(sle_team_cli_t *cli, const char *line);
void sle_team_cli_print_help(sle_team_cli_t *cli);

#ifdef __cplusplus
}
#endif

#endif
