#!/usr/bin/env python3
"""WS63 natural member-loss test.

This harness keeps member enrollment on the same natural-policy path as the
current minimal task book: boards are configured, the leader runs under
`allow=all`, and members join through fresh leader policy without reopening a
pairing window. It then simulates a single member signal loss and verifies the
full member set recovers without shrinking the intended count.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import sys
from typing import Optional

try:
    from automation.ws63.tools import ws63_four_board_relay_test as fb
    from automation.ws63.tools import ws63_link_cycle_test as lc
    from automation.ws63.tools import ws63_natural_relay_leave_recovery_test as nr
except ModuleNotFoundError:  # pragma: no cover - direct script execution
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(repo_root))
    from automation.ws63.tools import ws63_four_board_relay_test as fb
    from automation.ws63.tools import ws63_link_cycle_test as lc
    from automation.ws63.tools import ws63_natural_relay_leave_recovery_test as nr


DEFAULT_STABLE_POLLS = 3


def _progress(message: str) -> None:
    print(f"[five-board] {message}", flush=True)


def _dump_logs(peers: list[lc.Peer], out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for peer in peers:
        path = out_dir / f"{peer.name}_{peer.port}.log"
        with path.open("w", encoding="utf-8", errors="ignore") as handle:
            for item in peer.log:
                handle.write(item)
            tx_log = getattr(peer, "tx_log", [])
            if tx_log:
                handle.write("\n\n--- tx commands ---\n")
                for line in tx_log:
                    handle.write(f"[tx] {line}\n")


def _write_summary(path: pathlib.Path, summary: dict[str, object]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(summary, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def _select_disconnect_member(
    members: list[lc.Peer],
    member_ids: dict[str, int],
    requested_port: str,
) -> lc.Peer:
    if requested_port:
        wanted = requested_port.upper()
        for member in members:
            if member.port.upper() == wanted:
                return member
        raise RuntimeError(f"disconnect port {requested_port} is not in member ports")
    return members[0]


def _records_summary(records: fb.MemberRecords, member_ids: list[int]) -> str:
    return nr._records_line(records, member_ids)


def run(args: argparse.Namespace) -> int:
    peers: list[lc.Peer] = []
    leader_id: Optional[int] = None
    member_id_list: list[int] = []
    log_dir = pathlib.Path(args.log_dir) if args.log_dir else None
    summary_path = log_dir / "summary.json" if log_dir else None
    summary: dict[str, object] = {
        "leader_port": args.leader_port,
        "member_ports": args.member_ports,
        "expected_fw": args.expected_fw,
        "team_id": args.team_id,
        "channel": args.channel,
        "direct_cap": args.direct_cap,
        "disconnect_method": args.disconnect_method,
        "disconnect_member_port": args.disconnect_member_port,
        "log_dir": str(log_dir) if log_dir else "",
    }
    try:
        if len(args.member_ports) < 1:
            raise RuntimeError("at least one --member-ports value is required")

        if log_dir is not None:
            log_dir.mkdir(parents=True, exist_ok=True)

        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        members = [
            lc._open_peer(f"member{idx}", port, args.baudrate)
            for idx, port in enumerate(args.member_ports, start=1)
        ]
        peers = [leader, *members]

        _progress("drain serial boot logs")
        fb._drain_all(peers, args.initial_drain_s)

        statuses = fb._assert_fw(peers, peers, args.expected_fw)
        summary["initial_statuses"] = statuses
        leader_suffix = str(statuses["leader"]["selfSuffix"])
        leader_id = int(statuses["leader"]["routeId"])
        member_ids = {member.name: int(statuses[member.name]["routeId"]) for member in members}
        member_id_list = list(member_ids.values())
        peer_by_id = {member_ids[member.name]: member for member in members}
        all_route_ids = [leader_id, *member_id_list]
        if len(set(all_route_ids)) != len(all_route_ids):
            raise RuntimeError(f"route ids must be unique: {all_route_ids}")
        summary["leader_id"] = leader_id
        summary["member_ids"] = member_id_list

        if args.relay_member_ports:
            summary["ignored_relay_member_ports"] = args.relay_member_ports
            _progress(
                "legacy relay_member_ports ignored under natural enrollment; "
                "relay selection now follows fresh leader policy only"
            )

        if not args.no_clean_start:
            fb._clean_start_saved_config(
                peers,
                timeout_s=args.cmd_timeout_s,
                boot_timeout_s=args.boot_timeout_s,
                reboot_command=args.reboot_command,
            )

        leader_status = fb._configure_roles(
            leader,
            members,
            peers,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            direct_cap=args.direct_cap,
            relay_target=None,
            skip_direct_config=args.skip_direct_config,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        runtime_direct_cap = int(leader_status.get("runtimeDirectCap") or args.direct_cap)
        summary["leader_runtime_after_config"] = leader_status
        _progress(
            f"leader runtime: id={leader_id} suffix={leader_suffix} "
            f"direct_cap={runtime_direct_cap} relay_target={leader_status.get('runtimeRelayTarget')} "
            f"relay_count={leader_status.get('runtimeRelayCount')}"
        )

        route_log_start = len(leader.log)
        summary["enrollment_mode"] = "allow_all_no_pairing_window"
        initial_records, initial_relays, initial_topology = nr._wait_natural_enrollment(
            leader,
            members,
            peers,
            leader_id=leader_id,
            member_ids=member_id_list,
            direct_cap=runtime_direct_cap,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=route_log_start,
            stable_polls=DEFAULT_STABLE_POLLS,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            note="initial enrollment",
        )
        summary["initial_records"] = initial_records
        summary["initial_relays"] = initial_relays
        summary["initial_topology"] = initial_topology
        _progress(f"enrollment PASS: {_records_summary(initial_records, member_id_list)}")
        _progress("initial topology:\n" + "\n".join(initial_topology["tree_lines"]))

        if args.require_route_converged:
            initial_records = fb._wait_stable_final_topology(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_id_list,
                direct_cap=runtime_direct_cap,
                timeout_s=args.topology_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=route_log_start,
                stable_polls=DEFAULT_STABLE_POLLS,
                allow_any_converged=False,
            )
            summary["initial_strict_topology_records"] = initial_records
            _progress("initial strict route topology PASS")

        disconnect_member = _select_disconnect_member(members, member_ids, args.disconnect_member_port)
        disconnect_member_id = member_ids[disconnect_member.name]
        remaining_ids = [member_id for member_id in member_id_list if member_id != disconnect_member_id]
        remaining_peers = [leader, *[peer_by_id[member_id] for member_id in remaining_ids]]
        _progress(
            f"simulate disconnect: {args.disconnect_method} "
            f"{disconnect_member.name} {disconnect_member.port} id={disconnect_member_id}"
        )
        disconnect_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        disconnect_start = len(leader.log)
        member_boot_start = nr._drop_peer(
            disconnect_member,
            peers,
            method=args.disconnect_method,
            reboot_command=args.reboot_command,
            timeout_s=args.cmd_timeout_s,
            note=f"{disconnect_member.name} {args.disconnect_method}",
        )

        disconnect_offline_observed = False
        remaining_records: fb.MemberRecords = {}
        if args.disconnect_method == "leave":
            lc._wait_leader_offline_event(
                leader,
                peers,
                member_id=disconnect_member_id,
                timeout_s=args.offline_timeout_s,
                note=f"leader offline after {disconnect_member.name} {args.disconnect_method}",
                log_start=disconnect_start,
            )
            disconnect_offline_observed = True
            remaining_records = nr._wait_stable_remaining(
                leader,
                peers,
                leader_id=leader_id,
                remaining_ids=remaining_ids,
                left_ids=[disconnect_member_id],
                direct_cap=runtime_direct_cap,
                timeout_s=args.topology_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=disconnect_start,
                stable_polls=DEFAULT_STABLE_POLLS,
                note="member leave recovery",
            )
        else:
            _progress(
                "member reboot: skip mandatory offline gate; "
                "fast NV restore and leader-observed rejoin are validated next"
            )
            try:
                remaining_records = fb._wait_member_records(
                    leader,
                    peers,
                    expected={member_id: (1, None) for member_id in remaining_ids},
                    timeout_s=min(args.topology_timeout_s, args.offline_timeout_s),
                    poll_s=args.poll_interval_s,
                    note="remaining members online after signal-loss reboot",
                )
            except RuntimeError as exc:
                _progress(f"remaining topology evidence skipped while rebooting member: {exc}")

        remaining_topology = nr._topology_from_records(
            remaining_records,
            member_ids=remaining_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[disconnect_start:]),
        )
        nr._assert_no_route_regressions_scoped(
            remaining_peers,
            leader_id,
            log_start_by_name=disconnect_log_start_by_name,
            expected_source_ids=remaining_ids,
        )
        summary["member_leave"] = {
            "method": args.disconnect_method,
            "member_id": disconnect_member_id,
            "port": disconnect_member.port,
            "offline_observed": disconnect_offline_observed,
            "records": remaining_records,
            "relays": fb._relay_ids_from_records(remaining_records, remaining_ids),
            "topology": remaining_topology,
        }
        if disconnect_offline_observed:
            _progress(
                f"member {args.disconnect_method} offline observed: remaining online; "
                f"{_records_summary(remaining_records, remaining_ids)}"
            )
        else:
            _progress(
                f"member {args.disconnect_method} fast-rejoin path: remaining members checked; "
                f"{_records_summary(remaining_records, remaining_ids)}"
            )
        if remaining_topology["tree_lines"]:
            _progress("member drop topology:\n" + "\n".join(remaining_topology["tree_lines"]))

        _progress(f"member recover: {disconnect_member.name} {disconnect_member.port} id={disconnect_member_id}")
        rejoin_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        rejoin_start = len(leader.log)
        nr._recover_dropped_member(
            disconnect_member,
            peers,
            method=args.disconnect_method,
            reboot_log_start=member_boot_start,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        if args.disconnect_method == "reboot" and not args.skip_rejoin_check:
            fb._wait_leader_observes_member_reboot_rejoin(
                leader,
                peers,
                member_id=disconnect_member_id,
                leader_id=leader_id,
                timeout_s=args.rejoin_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=disconnect_start,
            )
            _progress(f"rejoin PASS: member={disconnect_member_id}")

        fb._wait_member_records(
            leader,
            peers,
            expected={disconnect_member_id: (1, None)},
            timeout_s=args.rejoin_timeout_s,
            poll_s=args.poll_interval_s,
            note=f"member recovered after {args.disconnect_method}",
        )
        nr._snapshot_peer_join_state(
            disconnect_member,
            peers,
            note=f"member recovered {disconnect_member.name}",
        )

        final_records = nr._wait_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_id_list,
            direct_cap=runtime_direct_cap,
            timeout_s=max(args.rejoin_timeout_s, args.topology_timeout_s),
            poll_s=args.poll_interval_s,
            log_start=rejoin_start,
            stable_polls=DEFAULT_STABLE_POLLS,
            note="member rejoin",
        )
        if args.require_route_converged and args.disconnect_method == "reboot" and not args.skip_rejoin_check:
            final_records = fb._wait_stable_final_topology(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_id_list,
                direct_cap=runtime_direct_cap,
                timeout_s=args.topology_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=rejoin_start,
                stable_polls=DEFAULT_STABLE_POLLS,
                allow_any_converged=False,
            )
            summary["final_strict_topology_records"] = final_records
            _progress("post-rejoin route topology PASS")

        final_relays = fb._relay_ids_from_records(final_records, member_id_list)
        final_topology = nr._topology_from_records(
            final_records,
            member_ids=member_id_list,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[rejoin_start:]),
        )
        nr._assert_no_route_regressions_scoped(
            peers,
            leader_id,
            log_start_by_name=rejoin_log_start_by_name,
            expected_source_ids=member_id_list,
        )
        summary["member_rejoin"] = {
            "member_id": disconnect_member_id,
            "records": final_records,
            "relays": final_relays,
            "topology": final_topology,
        }
        _progress(f"final members: {_records_summary(final_records, member_id_list)}")
        if final_topology["tree_lines"]:
            _progress("final topology:\n" + "\n".join(final_topology["tree_lines"]))

        summary["result"] = "PASS"
        _progress("PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        _progress(f"FAIL: {exc}")
        summary["result"] = "FAIL"
        summary["error"] = str(exc)
        return 1
    finally:
        if log_dir is not None:
            _dump_logs(peers, log_dir)
            if summary_path is not None:
                _write_summary(summary_path, summary)
        for peer in peers:
            try:
                peer.ser.close()
            except Exception:  # noqa: BLE001
                pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 natural member-loss test")
    parser.add_argument("--leader-port", required=True)
    parser.add_argument("--member-ports", nargs="+", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="v4.5.46-minimal")
    parser.add_argument("--team-id", type=int, default=7)
    parser.add_argument("--channel", type=int, default=33)
    parser.add_argument("--direct-cap", type=int, default=4)
    parser.add_argument("--skip-direct-config", action="store_true")
    parser.add_argument(
        "--relay-member-ports",
        nargs="*",
        default=[],
        help="legacy argument kept for compatibility; ignored under natural enrollment",
    )
    parser.add_argument("--require-route-converged", action="store_true")
    parser.add_argument("--disconnect-member-port", default="")
    parser.add_argument("--disconnect-method", choices=("reboot", "leave"), default="reboot")
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument("--no-clean-start", action="store_true")
    parser.add_argument("--skip-rejoin-check", action="store_true")
    parser.add_argument("--initial-drain-s", type=float, default=1.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=10.0)
    parser.add_argument("--boot-timeout-s", type=float, default=35.0)
    parser.add_argument("--state-timeout-s", type=float, default=45.0)
    parser.add_argument("--topology-timeout-s", type=float, default=60.0)
    parser.add_argument("--offline-timeout-s", type=float, default=30.0)
    parser.add_argument("--rejoin-timeout-s", type=float, default=60.0)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--log-dir", default="")
    return parser


def main() -> int:
    parser = build_parser()
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
