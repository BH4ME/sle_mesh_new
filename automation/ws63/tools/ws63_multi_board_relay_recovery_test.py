#!/usr/bin/env python3
"""WS63 multi-board natural relay enrollment and recovery test."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import time
from typing import Iterable

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


def _progress(message: str) -> None:
    print(f"[multi-board] {message}", flush=True)


def _records_line(records: fb.MemberRecords, member_ids: Iterable[int]) -> str:
    parts: list[str] = []
    for member_id in member_ids:
        record = records.get(member_id, {})
        parts.append(
            f"{member_id}:online={record.get('online')} relay={record.get('relay')} "
            f"tier={record.get('tier')} last={record.get('last_seen')}"
        )
    return "; ".join(parts)


def _find_leader_crash_events(leader: lc.Peer, *, log_start: int) -> list[str]:
    text = "".join(leader.log[log_start:])
    crash_rx = re.compile(r"(APP\|exception:[0-9A-Fa-f]+|APP\|Oops:[^\r\n]*|APP\|Reboot core:[^\r\n]*)")
    boot_rx = re.compile(r"(device_main_init:\s*0!|\[team\] boot fw=[^\r\n]*|APP\|dbg uart init ok\.)")
    task_rx = re.compile(r"task:([^\r\n]+)")
    mepc_rx = re.compile(r"mepc:0x([0-9A-Fa-f]+)")
    events: list[str] = []
    for match in crash_rx.finditer(text):
        window = text[match.start() : min(len(text), match.start() + 700)]
        details = [match.group(1)]
        task_match = task_rx.search(window)
        mepc_match = mepc_rx.search(window)
        if task_match is not None:
            details.append(f"task={task_match.group(1).strip()}")
        if mepc_match is not None:
            details.append(f"mepc=0x{mepc_match.group(1)}")
        events.append(" ".join(details))
    for match in boot_rx.finditer(text):
        events.append(f"runtime leader boot marker: {match.group(1)}")
    return events


def _direct_members_from_topology(
    topology: dict[str, object],
    *,
    member_ids: Iterable[int],
    leader_id: int,
) -> list[int]:
    parent_by_member = topology.get("parent_by_member", {})
    direct_members: list[int] = []
    for member_id in member_ids:
        parent_id = nr._topology_int_lookup(parent_by_member, member_id)
        if parent_id == leader_id:
            direct_members.append(member_id)
    return direct_members


def _assert_runtime_invariants(
    leader: lc.Peer,
    topology: dict[str, object],
    *,
    member_ids: Iterable[int],
    leader_id: int,
    direct_cap: int,
    stage: str,
    leader_log_start: int,
) -> None:
    errors: list[str] = []
    crash_events = _find_leader_crash_events(leader, log_start=leader_log_start)
    if crash_events:
        errors.append("leader crash/reboot observed: " + "; ".join(crash_events[:4]))
    direct_members = _direct_members_from_topology(topology, member_ids=member_ids, leader_id=leader_id)
    if len(direct_members) > direct_cap:
        errors.append(f"direct_count={len(direct_members)} exceeds direct_cap={direct_cap}: {direct_members}")
    if errors:
        raise RuntimeError(f"{stage} runtime invariant failed: " + " | ".join(errors))


def _wait_stable_online(
    leader: lc.Peer,
    peers: list[lc.Peer],
    member_ids: list[int],
    *,
    seconds: float,
    poll_s: float,
) -> fb.MemberRecords:
    _progress(f"stability dwell {seconds:.0f}s: polling members")
    end = time.time() + seconds
    last: fb.MemberRecords = {}
    poll_count = 0
    while time.time() < end:
        records = fb._wait_member_records(
            leader,
            peers,
            expected={member_id: (1, None) for member_id in member_ids},
            timeout_s=10.0,
            poll_s=poll_s,
            note="stability all members online",
        )
        last = records
        poll_count += 1
        relays = fb._relay_ids_from_records(records, member_ids)
        _progress(f"stable poll {poll_count}: relays={relays}; {_records_line(records, member_ids)}")
        time.sleep(min(5.0, max(0.0, end - time.time())))
    return last


def _query_initial_statuses(
    peers: list[lc.Peer],
    *,
    expected_fw: str,
) -> dict[str, dict[str, object]]:
    statuses: dict[str, dict[str, object]] = {}
    for peer in peers:
        status = fb._query_cfg(peer, peers, window_s=3.0, attempts=5)
        fw = status.get("fw")
        if expected_fw and fw != expected_fw:
            raise RuntimeError(f"{peer.name} firmware mismatch: expected {expected_fw}, got {fw}")
        statuses[peer.name] = status
        _progress(
            f"{peer.name} {peer.port}: fw={fw} suffix={status.get('selfSuffix')} "
            f"route={status.get('routeId')} role={status.get('runtimeRole')}"
        )
    return statuses


def run(args: argparse.Namespace) -> int:
    peers: list[lc.Peer] = []
    leader_id: int | None = None
    log_dir = pathlib.Path(args.log_dir)
    summary_path = log_dir / "summary.json"
    summary: dict[str, object] = {
        "leader_port": args.leader_port,
        "member_ports": args.member_ports,
        "expected_fw": args.expected_fw,
        "team_id": args.team_id,
        "channel": args.channel,
        "direct_cap": args.direct_cap,
        "relay_target": args.relay_target,
        "log_dir": str(log_dir),
    }
    try:
        if len(args.member_ports) < 2:
            raise RuntimeError("at least two member ports are required to exercise relay failover")

        log_dir.mkdir(parents=True, exist_ok=True)
        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        members = [
            lc._open_peer(f"member{idx}", port, args.baudrate)
            for idx, port in enumerate(args.member_ports, start=1)
        ]
        peers = [leader, *members]

        _progress("drain serial boot logs")
        fb._drain_all(peers, args.initial_drain_s)

        statuses = _query_initial_statuses(peers, expected_fw=args.expected_fw)
        summary["initial_statuses"] = statuses
        leader_suffix = str(statuses["leader"].get("selfSuffix"))
        leader_id = int(statuses["leader"].get("routeId"))
        member_ids = [int(statuses[member.name].get("routeId")) for member in members]
        peer_by_id = {member_id: peer for member_id, peer in zip(member_ids, members)}
        all_route_ids = [leader_id, *member_ids]
        if len(set(all_route_ids)) != len(all_route_ids):
            raise RuntimeError(f"route ids must be unique: {all_route_ids}")
        summary["leader_id"] = leader_id
        summary["member_ids"] = member_ids

        if not args.no_clean_start:
            fb._clean_start_saved_config(
                peers,
                timeout_s=args.cmd_timeout_s,
                boot_timeout_s=args.boot_timeout_s,
                reboot_command=args.reboot_command,
            )

        leader_runtime_log_start = len(leader.log)
        leader_status = fb._configure_roles(
            leader,
            members,
            peers,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            direct_cap=args.direct_cap,
            relay_target=args.relay_target,
            skip_direct_config=args.skip_direct_config,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        runtime_direct_cap = int(leader_status.get("runtimeDirectCap") or args.direct_cap)
        summary["leader_runtime_after_config"] = leader_status
        _progress(
            f"leader runtime: id={leader_id} suffix={leader_suffix} direct_cap={runtime_direct_cap} "
            f"relay_target={leader_status.get('runtimeRelayTarget')} "
            f"relay_count={leader_status.get('runtimeRelayCount')}"
        )

        route_log_start = len(leader.log)
        summary["enrollment_mode"] = "allow_all_no_pairing_window"
        records, relays, topology = nr._wait_natural_enrollment(
            leader,
            members,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=route_log_start,
            stable_polls=args.stable_polls,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            note="initial enrollment",
        )
        relays = fb._relay_ids_from_records(records, member_ids)
        summary["initial_records"] = records
        summary["initial_relays"] = relays
        summary["initial_topology"] = topology
        nr._assert_relays_are_leader_direct(topology, relay_ids=relays, leader_id=leader_id, stage="initial")
        _assert_runtime_invariants(
            leader,
            topology,
            member_ids=member_ids,
            leader_id=leader_id,
            direct_cap=runtime_direct_cap,
            stage="initial",
            leader_log_start=leader_runtime_log_start,
        )
        _progress(f"natural enrollment PASS: relays={relays}; {_records_line(records, member_ids)}")
        _progress("initial topology:\n" + "\n".join(topology["tree_lines"]))
        if args.relay_target is not None and len(relays) != args.relay_target:
            raise RuntimeError(
                f"unexpected initial relay count: expected={args.relay_target} observed={relays} records={records}"
            )
        if not relays:
            raise RuntimeError("no bootstrap relay observed after direct-cap pressure")

        stable_records = _wait_stable_online(
            leader,
            peers,
            member_ids,
            seconds=args.stability_s,
            poll_s=args.poll_interval_s,
        )
        summary["stability_records"] = stable_records

        non_relay_id = fb._prefer_non_relay_member_id(stable_records, member_ids)
        if non_relay_id is None:
            raise RuntimeError(f"no non-relay member available for member reboot: {stable_records}")
        non_relay = peer_by_id[non_relay_id]
        _progress(f"member reboot test: reboot non-relay {non_relay.name} {non_relay.port} id={non_relay_id}")
        member_reboot_start = len(leader.log)
        fb._send_cli_line(non_relay, args.reboot_command)
        fb._wait_pattern(
            non_relay,
            peers,
            pattern=fb.MEMBER_RESTORE_PATTERN,
            timeout_s=args.boot_timeout_s,
            note="member restore from NV",
        )
        fb._wait_leader_observes_member_reboot_rejoin(
            leader,
            peers,
            member_id=non_relay_id,
            leader_id=leader_id,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=member_reboot_start,
        )
        fb._wait_member_records(
            leader,
            peers,
            expected={non_relay_id: (1, None)},
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note="member rejoins after reboot",
        )
        fb._wait_peer_joined(non_relay, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        post_member_records = fb._wait_stable_final_topology(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=member_reboot_start,
            stable_polls=args.stable_polls,
            allow_any_converged=True,
        )
        fb._assert_no_route_regressions(peers, leader_id)
        post_member_relays = fb._relay_ids_from_records(post_member_records, member_ids)
        post_member_topology = nr._topology_from_records(
            post_member_records,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[member_reboot_start:]),
        )
        nr._assert_relays_are_leader_direct(
            post_member_topology,
            relay_ids=post_member_relays,
            leader_id=leader_id,
            stage="member reboot",
        )
        _assert_runtime_invariants(
            leader,
            post_member_topology,
            member_ids=member_ids,
            leader_id=leader_id,
            direct_cap=runtime_direct_cap,
            stage="member reboot",
            leader_log_start=leader_runtime_log_start,
        )
        summary["member_reboot"] = {
            "member_id": non_relay_id,
            "port": non_relay.port,
            "records": post_member_records,
            "relays": post_member_relays,
            "topology": post_member_topology,
        }
        _progress(f"member reboot PASS: relays={post_member_relays}; member={non_relay_id} rejoined")
        _progress("member reboot topology:\n" + "\n".join(post_member_topology["tree_lines"]))

        if not post_member_relays:
            raise RuntimeError(f"no relay available for relay reboot after member test: {post_member_records}")
        relay_id = post_member_relays[0]
        relay_peer = peer_by_id[relay_id]
        remaining_ids = [member_id for member_id in member_ids if member_id != relay_id]
        _progress(f"relay reboot test: reboot relay {relay_peer.name} {relay_peer.port} id={relay_id}")
        relay_drop_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        relay_reboot_start = len(leader.log)
        relay_restore_log_start = len(relay_peer.log)
        relay_offline_observed = True
        fb._send_cli_line(relay_peer, args.reboot_command)
        try:
            lc._wait_leader_offline_event(
                leader,
                peers,
                member_id=relay_id,
                timeout_s=args.offline_timeout_s,
                note="leader offline after relay reboot",
                log_start=relay_reboot_start,
            )
            _progress("leader offline observed after relay reboot")
        except RuntimeError as exc:
            relay_offline_observed = False
            _progress(f"leader offline event not observed before recovery topology converged: {exc}")
        relay_leave_records = nr._wait_stable_remaining(
            leader,
            peers,
            leader_id=leader_id,
            remaining_ids=remaining_ids,
            left_ids=[relay_id],
            direct_cap=runtime_direct_cap,
            timeout_s=args.failover_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=relay_reboot_start,
            stable_polls=max(2, min(args.stable_polls, 3)),
            note="relay leave recovery",
            require_left_ids_offline=False,
        )
        relays_after_leave = fb._relay_ids_from_records(relay_leave_records, remaining_ids)
        if len(remaining_ids) > runtime_direct_cap and not relays_after_leave:
            raise RuntimeError(f"relay reboot did not yield a replacement relay: {relay_leave_records}")
        relay_leave_topology = nr._topology_from_records(
            relay_leave_records,
            member_ids=remaining_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[relay_reboot_start:]),
        )
        nr._assert_no_route_regressions_scoped(
            [leader, *[peer_by_id[member_id] for member_id in remaining_ids]],
            leader_id,
            log_start_by_name=relay_drop_log_start_by_name,
            expected_source_ids=remaining_ids,
        )
        nr._assert_relays_are_leader_direct(
            relay_leave_topology,
            relay_ids=relays_after_leave,
            leader_id=leader_id,
            stage="relay leave",
        )
        _assert_runtime_invariants(
            leader,
            relay_leave_topology,
            member_ids=remaining_ids,
            leader_id=leader_id,
            direct_cap=runtime_direct_cap,
            stage="relay leave",
            leader_log_start=leader_runtime_log_start,
        )
        _progress(f"relay failover/bootstrap observed: replacement relays={relays_after_leave}")
        _progress("relay leave topology:\n" + "\n".join(relay_leave_topology["tree_lines"]))

        fb._wait_pattern(
            relay_peer,
            peers,
            pattern=fb.MEMBER_RESTORE_PATTERN,
            timeout_s=args.boot_timeout_s,
            note="relay restore from NV",
            log_start=relay_restore_log_start,
        )
        relay_rejoin_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        final_records = nr._wait_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=relay_reboot_start,
            stable_polls=max(2, min(args.stable_polls, 3)),
            note="relay rejoin",
        )
        nr._assert_no_route_regressions_scoped(
            peers,
            leader_id,
            log_start_by_name=relay_rejoin_log_start_by_name,
            expected_source_ids=member_ids,
        )
        final_relays = fb._relay_ids_from_records(final_records, member_ids)
        final_topology = nr._topology_from_records(
            final_records,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[relay_reboot_start:]),
        )
        nr._assert_relays_are_leader_direct(
            final_topology,
            relay_ids=final_relays,
            leader_id=leader_id,
            stage="relay rejoin",
        )
        nr._assert_old_relay_rejoins_as_child_or_member(
            final_topology,
            final_records,
            old_relay_id=relay_id,
            leader_id=leader_id,
            current_relays=final_relays,
            stage="relay rejoin",
        )
        _assert_runtime_invariants(
            leader,
            final_topology,
            member_ids=member_ids,
            leader_id=leader_id,
            direct_cap=runtime_direct_cap,
            stage="relay rejoin",
            leader_log_start=leader_runtime_log_start,
        )
        policy = fb._summarize_policy(final_records, relay_id, remaining_ids)
        summary["relay_reboot"] = {
            "relay_id": relay_id,
            "port": relay_peer.port,
            "leader_offline_observed": relay_offline_observed,
            "replacement_relays": relays_after_leave,
            "relay_leave_records": relay_leave_records,
            "relay_leave_topology": relay_leave_topology,
            "final_records": final_records,
            "final_relays": final_relays,
            "final_topology": final_topology,
            "policy": policy,
        }
        summary["result"] = "PASS"
        _progress(f"relay recovery PASS: {policy}; final_relays={final_relays}")
        _progress("final topology:\n" + "\n".join(final_topology["tree_lines"]))
        _progress("PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        _progress(f"FAIL: {exc}")
        summary["result"] = "FAIL"
        summary["error"] = str(exc)
        if leader_id is not None:
            regressions = fb._find_route_regression_events(peers, leader_id)
            if regressions:
                _progress("route regression evidence: " + "; ".join(regressions[:5]))
                summary["route_regressions"] = regressions[:20]
        return 1
    finally:
        if peers:
            try:
                fb._dump_logs(peers, log_dir)
                _progress(f"logs saved: {log_dir}")
            except Exception as dump_exc:  # noqa: BLE001
                _progress(f"WARN: failed to dump logs: {dump_exc}")
        try:
            log_dir.mkdir(parents=True, exist_ok=True)
            summary_path.write_text(
                json.dumps(summary, indent=2, ensure_ascii=False, default=str),
                encoding="utf-8",
            )
            _progress(f"summary saved: {summary_path}")
        except Exception as summary_exc:  # noqa: BLE001
            _progress(f"WARN: failed to write summary: {summary_exc}")
        for peer in peers:
            try:
                peer.ser.close()
            except Exception:  # noqa: BLE001
                pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 live multi-board natural relay recovery test")
    parser.add_argument("--leader-port", required=True)
    parser.add_argument("--member-ports", nargs="+", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="v4.5.64-minimal")
    parser.add_argument("--team-id", type=int, default=7)
    parser.add_argument("--channel", type=int, default=33)
    parser.add_argument("--direct-cap", type=int, default=4)
    parser.add_argument("--relay-target", type=int, default=None)
    parser.add_argument("--skip-direct-config", action="store_true")
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument("--no-clean-start", action="store_true")
    parser.add_argument("--initial-drain-s", type=float, default=2.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=20.0)
    parser.add_argument("--boot-timeout-s", type=float, default=85.0)
    parser.add_argument("--state-timeout-s", type=float, default=100.0)
    parser.add_argument("--route-timeout-s", type=float, default=140.0)
    parser.add_argument("--offline-timeout-s", type=float, default=45.0)
    parser.add_argument("--failover-timeout-s", type=float, default=120.0)
    parser.add_argument("--stability-s", type=float, default=45.0)
    parser.add_argument("--stable-polls", type=int, default=4)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--log-dir", required=True)
    return parser


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
