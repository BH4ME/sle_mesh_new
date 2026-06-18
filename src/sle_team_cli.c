#include "sle_team_cli.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static void sle_team_cli_puts(sle_team_cli_t *cli, const char *text)
{
    if (cli != NULL && cli->print != NULL) {
        cli->print(cli->user_ctx, text);
    }
}

/* Small printf wrapper so all CLI output goes through the board print callback. */
static void sle_team_cli_printf(sle_team_cli_t *cli, const char *fmt, ...)
{
    char buf[256];
    va_list ap;

    if (cli == NULL || cli->print == NULL) {
        return;
    }

    va_start(ap, fmt);
    (void)vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    cli->print(cli->user_ctx, buf);
}

/* Parse unsigned integers from CLI text while enforcing a numeric range. */
static int parse_u32(const char *s, uint32_t min_value, uint32_t max_value, uint32_t *out)
{
    char *end = NULL;
    unsigned long v;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    v = strtoul(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > max_value || v < min_value) {
        return -1;
    }
    *out = (uint32_t)v;
    return 0;
}

/* Parse signed integers for coordinates, RSSI, and other signed fields. */
static int parse_i32(const char *s, int32_t min_value, int32_t max_value, int32_t *out)
{
    char *end = NULL;
    long v;

    if (s == NULL || out == NULL) {
        return -1;
    }

    errno = 0;
    v = strtol(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v < min_value || v > max_value) {
        return -1;
    }
    *out = (int32_t)v;
    return 0;
}

/* Convert core error codes into short human-readable CLI reasons. */
static const char *sle_team_cli_err_name(int ret)
{
    switch (ret) {
        case SLE_TEAM_OK:
            return "OK";
        case SLE_TEAM_ERR_ARG:
            return "ARG";
        case SLE_TEAM_ERR_BUF:
            return "BUF";
        case SLE_TEAM_ERR_FORMAT:
            return "FORMAT";
        case SLE_TEAM_ERR_UNSUPPORTED:
            return "NOT_READY";
        default:
            return "ERR";
    }
}

/* Uniform success/fail printout for all packet-sending commands. */
static void sle_team_cli_print_send_result(sle_team_cli_t *cli, const char *type, uint8_t dst_id, int ret)
{
    if (ret == SLE_TEAM_OK) {
        sle_team_cli_printf(cli, "sle_tx_ok type=%s dst=%u", type, dst_id);
    } else {
        sle_team_cli_printf(cli, "sle_tx_fail type=%s dst=%u ret=%d reason=%s",
            type, dst_id, ret, sle_team_cli_err_name(ret));
    }
}

/* CLI uses a short label so repeated member lines are easier to scan. */
static void sle_team_cli_format_member_label(const sle_team_member_record_t *member, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (member != NULL && member->mac_ready != 0U) {
        (void)snprintf(out, out_size, "M%02X%02X", member->mac[4], member->mac[5]);
        return;
    }
    if (member != NULL) {
        (void)snprintf(out, out_size, "M%u", member->member_id);
        return;
    }
    (void)snprintf(out, out_size, "M--");
}

/* Pending approvals get the same compact label style as active members. */
static void sle_team_cli_format_pending_label(const sle_team_pending_member_t *pending, char *out, size_t out_size)
{
    if (out == NULL || out_size == 0U) {
        return;
    }
    if (pending != NULL && pending->mac_ready != 0U) {
        (void)snprintf(out, out_size, "M%02X%02X", pending->mac[4], pending->mac[5]);
        return;
    }
    if (pending != NULL) {
        (void)snprintf(out, out_size, "M%u", pending->member_id);
        return;
    }
    (void)snprintf(out, out_size, "M--");
}

void sle_team_cli_init(sle_team_cli_t *cli, sle_team_node_t *node, sle_team_cli_print_fn print, void *user_ctx)
{
    if (cli == NULL) {
        return;
    }
    cli->node = node;
    cli->print = print;
    cli->user_ctx = user_ctx;
}

void sle_team_cli_print_help(sle_team_cli_t *cli)
{
    sle_team_cli_puts(cli, "commands:");
    sle_team_cli_puts(cli, "  help");
    sle_team_cli_puts(cli, "  hello [dst]");
    sle_team_cli_puts(cli, "  hb [dst] [battery] [rssi] [fix]");
    sle_team_cli_puts(cli, "  pos [dst] [lat_e6] [lon_e6] [speed] [heading] [battery] [fix] [sat]");
    sle_team_cli_puts(cli, "  alert [dst] [lost_id] [reason] [last_lat] [last_lon] [last_ts]");
    sle_team_cli_puts(cli, "  config [dst]");
    sle_team_cli_puts(cli, "  ack [dst] [ack_seq] [acked_type] [status]");
    sle_team_cli_puts(cli, "  members");
    sle_team_cli_puts(cli, "  allow [all|only <id...>|add <id>|del <id>]");
    sle_team_cli_puts(cli, "  pairing [start|stop|approve <id> [relay|norelay]|pending]");
    sle_team_cli_puts(cli, "  join <team> <leader> <channel>");
    sle_team_cli_puts(cli, "  leave");
    sle_team_cli_puts(cli, "  led help");
    sle_team_cli_puts(cli, "  rgb help");
    sle_team_cli_puts(cli, "  buzz help");
    sle_team_cli_puts(cli, "  disp help");
    sle_team_cli_puts(cli, "  state");
}

void sle_team_cli_handle_line(sle_team_cli_t *cli, const char *line)
{
    char local[192];
    char *argv[12];
    int argc = 0;
    char *tok;
    uint32_t v[8];
    int32_t sv[4];
    int i;
    sle_team_pos_body_t pos;
    sle_team_alert_body_t alert;
    const sle_team_member_record_t *member;

    if (cli == NULL || cli->node == NULL || line == NULL) {
        return;
    }

    (void)snprintf(local, sizeof(local), "%s", line);
    tok = strtok(local, " \r\n\t");
    while (tok != NULL && argc < (int)(sizeof(argv) / sizeof(argv[0]))) {
        argv[argc++] = tok;
        tok = strtok(NULL, " \r\n\t");
    }

    if (argc == 0) {
        return;
    }

    /* Each command is a thin shell around one core packet/state-machine call. */
    if (strcmp(argv[0], "help") == 0) {
        sle_team_cli_print_help(cli);
        return;
    }

    if (strcmp(argv[0], "hello") == 0) {
        uint8_t dst = (argc >= 2) ? (uint8_t)strtoul(argv[1], NULL, 0) : cli->node->cfg.leader_id;
        sle_team_cli_print_send_result(cli, "HELLO", dst, sle_team_node_send_hello(cli->node, dst));
        return;
    }

    if (strcmp(argv[0], "hb") == 0) {
        if (argc < 5) {
            sle_team_cli_puts(cli, "usage: hb [dst] [battery] [rssi] [fix]");
            return;
        }
        if (parse_u32(argv[1], 0U, 255U, &v[0]) != 0 ||
            parse_u32(argv[2], 0U, 100U, &v[1]) != 0 ||
            parse_i32(argv[3], -128, 127, &sv[0]) != 0 ||
            parse_u32(argv[4], 0U, 255U, &v[2]) != 0) {
            sle_team_cli_puts(cli, "bad number");
            return;
        }
        sle_team_cli_print_send_result(cli, "HEARTBEAT", (uint8_t)v[0],
            sle_team_node_send_heartbeat(cli->node, (uint8_t)v[0], (uint8_t)v[1], (int8_t)sv[0], (uint8_t)v[2]));
        return;
    }

    if (strcmp(argv[0], "pos") == 0) {
        if (argc < 9) {
            sle_team_cli_puts(cli, "usage: pos [dst] [lat_e6] [lon_e6] [speed] [heading] [battery] [fix] [sat]");
            return;
        }
        if (parse_u32(argv[1], 0U, 255U, &v[0]) != 0 ||
            parse_i32(argv[2], INT32_MIN, INT32_MAX, &sv[0]) != 0 ||
            parse_i32(argv[3], INT32_MIN, INT32_MAX, &sv[1]) != 0 ||
            parse_u32(argv[4], 0U, UINT16_MAX, &v[1]) != 0 ||
            parse_u32(argv[5], 0U, UINT16_MAX, &v[2]) != 0 ||
            parse_u32(argv[6], 0U, 100U, &v[3]) != 0 ||
            parse_u32(argv[7], 0U, 255U, &v[4]) != 0 ||
            parse_u32(argv[8], 0U, 255U, &v[5]) != 0) {
            sle_team_cli_puts(cli, "bad number");
            return;
        }
        memset(&pos, 0, sizeof(pos));
        pos.latitude_e6 = sv[0];
        pos.longitude_e6 = sv[1];
        pos.speed_cms = (uint16_t)v[1];
        pos.heading_deg = (uint16_t)v[2];
        pos.battery_percent = (uint8_t)v[3];
        pos.fix_status = (uint8_t)v[4];
        pos.sat_count = (uint8_t)v[5];
        sle_team_cli_print_send_result(cli, "POS_REPORT", (uint8_t)v[0],
            sle_team_node_send_position(cli->node, (uint8_t)v[0], &pos));
        return;
    }

    if (strcmp(argv[0], "alert") == 0) {
        if (argc < 7) {
            sle_team_cli_puts(cli, "usage: alert [dst] [lost_id] [reason] [last_lat] [last_lon] [last_ts]");
            return;
        }
        if (parse_u32(argv[1], 0U, 255U, &v[0]) != 0 ||
            parse_u32(argv[2], 0U, 255U, &v[1]) != 0 ||
            parse_u32(argv[3], 0U, 255U, &v[2]) != 0 ||
            parse_i32(argv[4], INT32_MIN, INT32_MAX, &sv[0]) != 0 ||
            parse_i32(argv[5], INT32_MIN, INT32_MAX, &sv[1]) != 0 ||
            parse_u32(argv[6], 0U, UINT32_MAX, &v[3]) != 0) {
            sle_team_cli_puts(cli, "bad number");
            return;
        }
        memset(&alert, 0, sizeof(alert));
        alert.lost_member_id = (uint8_t)v[1];
        alert.reason = (uint8_t)v[2];
        alert.last_latitude_e6 = sv[0];
        alert.last_longitude_e6 = sv[1];
        alert.last_report_s = v[3];
        sle_team_cli_print_send_result(cli, "ALERT", (uint8_t)v[0],
            sle_team_node_send_alert(cli->node, (uint8_t)v[0], &alert));
        return;
    }

    if (strcmp(argv[0], "config") == 0) {
        uint8_t dst = (argc >= 2) ? (uint8_t)strtoul(argv[1], NULL, 0) : SLE_TEAM_BROADCAST_ID;
        sle_team_cli_print_send_result(cli, "CONFIG", dst, sle_team_node_send_config(cli->node, dst));
        return;
    }

    if (strcmp(argv[0], "ack") == 0) {
        if (argc < 5) {
            sle_team_cli_puts(cli, "usage: ack [dst] [ack_seq] [acked_type] [status]");
            return;
        }
        if (parse_u32(argv[1], 0U, 255U, &v[0]) != 0 ||
            parse_u32(argv[2], 0U, UINT16_MAX, &v[1]) != 0 ||
            parse_u32(argv[3], 0U, 255U, &v[2]) != 0 ||
            parse_u32(argv[4], 0U, 255U, &v[3]) != 0) {
            sle_team_cli_puts(cli, "bad number");
            return;
        }
        sle_team_cli_print_send_result(cli, "ACK", (uint8_t)v[0],
            sle_team_node_send_ack(cli->node, (uint8_t)v[0], (uint16_t)v[1], (uint8_t)v[2], (uint8_t)v[3]));
        return;
    }

    if (strcmp(argv[0], "members") == 0) {
        /* Show every record that still matters: online, pending, or position-known. */
        for (i = 0; i < (int)SLE_TEAM_MAX_MEMBERS; i++) {
            char member_label[8];
            member = &cli->node->members[i];
            if (member->online != 0U || member->policy_pending != 0U || member->position_valid != 0U) {
                sle_team_cli_format_member_label(member, member_label, sizeof(member_label));
                sle_team_cli_printf(cli,
                    "member=%u label=%s role=%u online=%u pending=%u battery=%u fix=%u pos_valid=%u lat=%ld lon=%ld speed=%u heading=%u sat=%u rssi=%d mac=%02X%02X ready=%u relay=%u tier=%u max_down=%u parent=%u next=%u child_count=%u last_seq=%u last_seen=%lu",
                    member->member_id, member_label, member->role, member->online, member->policy_pending,
                    member->battery_percent, member->fix_status, member->position_valid, (long)member->latitude_e6,
                    (long)member->longitude_e6, member->speed_cms, member->heading_deg, member->sat_count,
                    member->last_rssi_dbm, member->mac[4], member->mac[5], member->mac_ready,
                    member->relay_allowed, member->relay_tier, member->max_downstream,
                    member->parent_id, member->next_hop_id, member->child_count, member->last_seq,
                    (unsigned long)member->last_seen_s);
            }
        }
        return;
    }

    if (strcmp(argv[0], "allow") == 0) {
        uint8_t ids[SLE_TEAM_MAX_MEMBERS];
        uint8_t count;
        int ret;

        if (argc == 1) {
            /* No args means read back the current admission filter. */
            if (cli->node->cfg.member_filter_enabled == 0U) {
                sle_team_cli_puts(cli, "allow=all");
            } else {
                sle_team_cli_printf(cli, "allow=only count=%u", cli->node->cfg.allowed_member_count);
                for (i = 0; i < (int)cli->node->cfg.allowed_member_count; i++) {
                    sle_team_cli_printf(cli, "allow member=%u", cli->node->cfg.allowed_member_ids[i]);
                }
            }
            return;
        }

        if (strcmp(argv[1], "all") == 0) {
            ret = sle_team_node_allow_all_members(cli->node);
            sle_team_cli_printf(cli, "allow all ret=%d", ret);
            return;
        }

        if (strcmp(argv[1], "only") == 0) {
            if (argc < 3 || argc - 2 > (int)SLE_TEAM_MAX_MEMBERS) {
                sle_team_cli_puts(cli, "usage: allow only <id...>");
                return;
            }
            count = 0U;
            for (i = 2; i < argc; i++) {
                if (parse_u32(argv[i], 1U, 254U, &v[0]) != 0) {
                    sle_team_cli_puts(cli, "bad member id");
                    return;
                }
                ids[count++] = (uint8_t)v[0];
            }
            ret = sle_team_node_set_allowed_members(cli->node, ids, count);
            sle_team_cli_printf(cli, "allow only count=%u ret=%d", count, ret);
            return;
        }

        if (strcmp(argv[1], "add") == 0 || strcmp(argv[1], "del") == 0) {
            if (argc < 3 || parse_u32(argv[2], 1U, 254U, &v[0]) != 0) {
                sle_team_cli_puts(cli, "usage: allow add|del <id>");
                return;
            }
            ret = strcmp(argv[1], "add") == 0 ?
                sle_team_node_add_allowed_member(cli->node, (uint8_t)v[0]) :
                sle_team_node_remove_allowed_member(cli->node, (uint8_t)v[0]);
            sle_team_cli_printf(cli, "allow %s member=%u ret=%d", argv[1], (uint8_t)v[0], ret);
            return;
        }

        sle_team_cli_puts(cli, "usage: allow [all|only <id...>|add <id>|del <id>]");
        return;
    }

    if (strcmp(argv[0], "pairing") == 0) {
        int ret;

        if (argc < 2 || strcmp(argv[1], "pending") == 0) {
            /* Pending list shows HELLOs waiting for a leader approval decision. */
            for (i = 0; i < (int)SLE_TEAM_MAX_MEMBERS; i++) {
                const sle_team_pending_member_t *pending = &cli->node->pending_members[i];
                if (pending->active != 0U) {
                    char pending_label[8];

                    sle_team_cli_format_pending_label(pending, pending_label, sizeof(pending_label));
                    sle_team_cli_printf(cli, "pending member=%u role=%u battery=%u mac=%02X%02X ready=%u last_seen=%lu label=%s",
                        pending->member_id, pending->role, pending->battery_percent,
                        pending->mac[4], pending->mac[5], pending->mac_ready,
                        (unsigned long)pending->last_seen_s, pending_label);
                }
            }
            return;
        }
        if (strcmp(argv[1], "start") == 0) {
            ret = sle_team_node_pairing_start(cli->node);
            sle_team_cli_printf(cli, "pairing start ret=%d", ret);
            return;
        }
        if (strcmp(argv[1], "stop") == 0) {
            ret = sle_team_node_pairing_stop(cli->node);
            sle_team_cli_printf(cli, "pairing stop ret=%d", ret);
            return;
        }
        if (strcmp(argv[1], "approve") == 0) {
            uint8_t relay_allowed = 0U;

            if (argc < 3 || parse_u32(argv[2], 1U, 254U, &v[0]) != 0) {
                sle_team_cli_puts(cli, "usage: pairing approve <id> [relay|norelay]");
                return;
            }
            if (argc >= 4) {
                if (strcmp(argv[3], "relay") == 0) {
                    relay_allowed = 1U;
                } else if (strcmp(argv[3], "norelay") == 0) {
                    relay_allowed = 0U;
                } else {
                    sle_team_cli_puts(cli, "usage: pairing approve <id> [relay|norelay]");
                    return;
                }
            }
            ret = sle_team_node_pairing_approve_with_relay(cli->node, (uint8_t)v[0], relay_allowed);
            sle_team_cli_printf(cli, "pairing approve member=%u relay=%u ret=%d",
                (uint8_t)v[0], relay_allowed, ret);
            return;
        }
        sle_team_cli_puts(cli, "usage: pairing [start|stop|approve <id> [relay|norelay]|pending]");
        return;
    }

    if (strcmp(argv[0], "join") == 0) {
        int ret;

        if (argc < 4 || parse_u32(argv[1], 1U, 254U, &v[0]) != 0 ||
            parse_u32(argv[2], 1U, 254U, &v[1]) != 0 ||
            parse_u32(argv[3], 0U, 255U, &v[2]) != 0) {
            sle_team_cli_puts(cli, "usage: join <team> <leader> <channel>");
            return;
        }
        ret = sle_team_node_member_select_leader(cli->node, (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2]);
        sle_team_cli_printf(cli, "join team=%u leader=%u channel=%u ret=%d",
            (uint8_t)v[0], (uint8_t)v[1], (uint8_t)v[2], ret);
        return;
    }

    if (strcmp(argv[0], "leave") == 0) {
        /* Core sends the leave alert if joined, then clears local member state. */
        int ret = sle_team_node_member_leave(cli->node);
        sle_team_cli_printf(cli, "leave ret=%d", ret);
        return;
    }

    if (strcmp(argv[0], "state") == 0) {
        sle_team_cli_printf(cli,
            "team=%u self=%u leader=%u role=%u state=%u joined=%u seq=%u pairing=%u allow=%s allow_count=%u",
            cli->node->cfg.team_id, cli->node->cfg.self_id, cli->node->cfg.leader_id,
            cli->node->cfg.role, cli->node->state, cli->node->joined, cli->node->next_seq,
            cli->node->cfg.pairing_enabled,
            cli->node->cfg.member_filter_enabled != 0U ? "only" : "all",
            cli->node->cfg.allowed_member_count);
        return;
    }

    sle_team_cli_puts(cli, "unknown command, type help");
}
