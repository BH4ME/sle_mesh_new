#!/usr/bin/env python3
"""WS63 natural relay soak test with a simultaneous two-member signal-loss drop.

This live-hardware test keeps relay election automatic. It configures only the
leader direct capacity and optional relay target, enrolls members naturally,
then drops two members in one burst. The default drop method is a soft reboot,
which models the leader losing the members' radio signal without an active
team leave, then verifies the remaining topology recovers before the rebooted
members restore from NV and rejoin automatically.
"""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import time
from typing import Iterable, Optional

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


RouteMetrics = dict[str, int]

_ROUTE_METRICS_RE = re.compile(
    r"route metrics active=(?P<active>\d+) direct=(?P<direct>\d+) relayed=(?P<relayed>\d+) "
    r"stale=(?P<stale>\d+) unreachable=(?P<unreachable>\d+) plan=(?P<plan>\d+) "
    r"converged=(?P<converged>\d+) epoch=(?P<epoch>\d+)"
)


def _progress(message: str) -> None:
    print(f"[soak-double] {message}", flush=True)


def _records_line(records: fb.MemberRecords, member_ids: Iterable[int]) -> str:
    return nr._records_line(records, member_ids)


def _latest_route_metrics(text: str) -> Optional[RouteMetrics]:
    latest: Optional[RouteMetrics] = None
    for match in _ROUTE_METRICS_RE.finditer(text):
        latest = {key: int(value) for key, value in match.groupdict().items()}
    return latest


def _route_metrics_ok(metrics: Optional[RouteMetrics], expected_active: int) -> bool:
    if metrics is None:
        return False
    return (
        metrics.get("active") == expected_active
        and metrics.get("stale") == 0
        and metrics.get("unreachable") == 0
        and metrics.get("plan") == 0
        and metrics.get("converged") == 1
    )


def _wait_relay_count(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    member_ids: list[int],
    relay_target: int,
    timeout_s: float,
    poll_s: float,
    note: str,
) -> fb.MemberRecords:
    end = time.time() + timeout_s
    last: fb.MemberRecords = {}
    while time.time() < end:
        records = fb._query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last = records
        online = [member_id for member_id in member_ids if records.get(member_id, {}).get("online") == 1]
        relays = fb._relay_ids_from_records(records, member_ids)
        if len(online) == len(member_ids) and len(relays) == relay_target:
            return records
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(
        f"timeout waiting for relay target during {note}: target={relay_target} "
        f"last_relays={fb._relay_ids_from_records(last, member_ids)} last={last}"
    )


def _wait_initial_topology_ready(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    leader_id: int,
    member_ids: list[int],
    direct_cap: int,
    relay_target: int,
    timeout_s: float,
    poll_s: float,
    log_start: int,
    stable_polls: int = 2,
) -> fb.MemberRecords:
    records = fb._wait_member_records(
        leader,
        peers,
        expected={member_id: (1, None) for member_id in member_ids},
        timeout_s=timeout_s,
        poll_s=poll_s,
        note="all members online after enrollment",
    )
    records = _wait_relay_count(
        leader,
        peers,
        member_ids=member_ids,
        relay_target=relay_target,
        timeout_s=timeout_s,
        poll_s=poll_s,
        note="initial enrollment",
    )
    advisory_timeout_s = min(timeout_s, max(6.0, poll_s * max(4, stable_polls + 1)))
    try:
        return fb._wait_stable_final_topology(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=direct_cap,
            timeout_s=advisory_timeout_s,
            poll_s=poll_s,
            log_start=log_start,
            stable_polls=max(2, stable_polls),
            allow_any_converged=True,
        )
    except RuntimeError as exc:
        _progress(f"initial topology route gate not fully refreshed, fallback to member/relay evidence: {exc}")
        return records


def _children_by_parent(topology: dict[str, object], member_ids: list[int]) -> dict[int, list[int]]:
    parent_by_member = topology.get("parent_by_member", {})
    children: dict[int, list[int]] = {}
    if not isinstance(parent_by_member, dict):
        return children
    for raw_member_id, raw_parent_id in parent_by_member.items():
        try:
            member_id = int(raw_member_id)
            parent_id = int(raw_parent_id)
        except (TypeError, ValueError):
            continue
        if member_id in member_ids:
            children.setdefault(parent_id, []).append(member_id)
    for child_ids in children.values():
        child_ids.sort()
    return children


def _choose_double_drop(
    records: fb.MemberRecords,
    member_ids: list[int],
    topology: dict[str, object],
    *,
    selection: str = "non-relay-pair",
) -> tuple[int, int, str]:
    relays = fb._relay_ids_from_records(records, member_ids)
    if not relays:
        raise RuntimeError(f"no relay available for double-drop: {records}")

    non_relays = [
        member_id
        for member_id in member_ids
        if member_id not in relays and records.get(member_id, {}).get("online") == 1
    ]
    if selection == "non-relay-pair":
        if len(non_relays) < 2:
            raise RuntimeError(f"need two online non-relay members for double-drop: {records}")
        return non_relays[0], non_relays[1], "non-relay-pair"

    children = _children_by_parent(topology, member_ids)
    relay_with_child = sorted(relays, key=lambda member_id: len(children.get(member_id, [])), reverse=True)[0]
    relay_children = [member_id for member_id in children.get(relay_with_child, []) if member_id != relay_with_child]
    if selection == "relay-plus-child" and relay_children:
        return relay_with_child, relay_children[0], "relay-plus-child"
    if selection == "relay-plus-child":
        raise RuntimeError(f"no relay child available for double-drop: {topology}")

    if selection == "relay-plus-member":
        for member_id in non_relays:
            if member_id != relay_with_child:
                return relay_with_child, member_id, "relay-plus-member"
        raise RuntimeError(f"no online non-relay member available for relay-plus-member drop: {records}")

    if selection != "auto-legacy":
        raise RuntimeError(f"unsupported drop selection: {selection}")

    if relay_children:
        return relay_with_child, relay_children[0], "relay-plus-child"
    for member_id in non_relays:
        if member_id != relay_with_child:
            return relay_with_child, member_id, "relay-plus-member"
    for member_id in relays:
        if member_id != relay_with_child and records.get(member_id, {}).get("online") == 1:
            return relay_with_child, member_id, "relay-plus-relay"
    raise RuntimeError(f"could not choose a second online member for double-drop: {records}")


def _send_leave_pair(drop_peers: list[lc.Peer], peers: list[lc.Peer], timeout_s: float) -> None:
    starts = {peer.name: len(peer.log) for peer in drop_peers}
    for peer in drop_peers:
        fb._send_cli_line(peer, "leave")
    for peer in drop_peers:
        fb._wait_pattern(
            peer,
            peers,
            pattern=r"leave ret=0",
            timeout_s=timeout_s,
            note=f"{peer.name} simultaneous leave accepted",
            log_start=starts[peer.name],
        )


def _send_signal_loss_pair(drop_peers: list[lc.Peer], command: str) -> dict[str, int]:
    starts = {peer.name: len(peer.log) for peer in drop_peers}
    for peer in drop_peers:
        fb._send_cli_line(peer, command)
    return starts


def _wait_dropped_pair_rejoined(
    leader: lc.Peer,
    peers: list[lc.Peer],
    drop_peers: list[lc.Peer],
    *,
    leader_id: int,
    drop_ids: list[int],
    timeout_s: float,
    poll_s: float,
    log_start: int,
) -> fb.MemberRecords:
    records: fb.MemberRecords = {}
    for drop_id in drop_ids:
        records = fb._wait_leader_observes_member_reboot_rejoin(
            leader,
            peers,
            member_id=drop_id,
            leader_id=leader_id,
            timeout_s=timeout_s,
            poll_s=poll_s,
            log_start=log_start,
        )
    for peer in drop_peers:
        fb._wait_peer_joined(peer, peers, timeout_s=timeout_s, poll_s=poll_s)
    return records


def _find_crash_events(peers: Iterable[lc.Peer]) -> list[str]:
    events: list[str] = []
    crash_rx = re.compile(r"(APP\|exception:[0-9A-Fa-f]+|APP\|Oops:[^\r\n]*|APP\|Reboot core:[^\r\n]*)")
    task_rx = re.compile(r"task:([^\r\n]+)")
    mepc_rx = re.compile(r"mepc:0x([0-9A-Fa-f]+)")
    for peer in peers:
        text = "".join(peer.log)
        matches = list(crash_rx.finditer(text))
        for match in matches:
            window = text[match.start() : min(len(text), match.start() + 700)]
            task_match = task_rx.search(window)
            mepc_match = mepc_rx.search(window)
            details = [match.group(1)]
            if task_match is not None:
                details.append(f"task={task_match.group(1).strip()}")
            if mepc_match is not None:
                details.append(f"mepc=0x{mepc_match.group(1)}")
            events.append(f"{peer.name} {peer.port}: " + " ".join(details))
    return events


def _parent_by_member_from_records(
    records: fb.MemberRecords,
    member_ids: list[int],
    *,
    leader_id: int,
) -> dict[int, int]:
    parent_by_member: dict[int, int] = {}
    for member_id in member_ids:
        record = records.get(member_id)
        if record is None or record.get("online") != 1:
            continue
        parent_id = record.get("parent_id")
        try:
            parent_int = int(parent_id)
        except (TypeError, ValueError):
            parent_int = leader_id
        if parent_int == 0:
            parent_int = leader_id
        parent_by_member[member_id] = parent_int
    return parent_by_member


def _wait_soak(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    leader_id: int,
    member_ids: list[int],
    expected_relay_count: int,
    duration_s: float,
    poll_s: float,
    report_interval_s: float,
    max_bad_polls: int,
    log_start: int,
    regression_log_start_by_name: dict[str, int],
    label: str,
    expected_parent_by_member: Optional[dict[int, int]] = None,
    expected_relay_ids: Optional[list[int]] = None,
) -> dict[str, object]:
    _progress(f"{label}: soak start duration={duration_s:.0f}s expected_relays={expected_relay_count}")
    end = time.time() + duration_s
    next_report = time.time()
    poll_count = 0
    bad_streak = 0
    bad_samples: list[dict[str, object]] = []
    last_records: fb.MemberRecords = {}
    last_relays: list[int] = []
    last_metrics = _latest_route_metrics("".join(leader.log[log_start:]))
    route_metrics_missing_polls = 0
    route_metrics_warned = False

    while time.time() < end:
        records = fb._query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last_records = records
        online = [member_id for member_id in member_ids if records.get(member_id, {}).get("online") == 1]
        relays = fb._relay_ids_from_records(records, member_ids)
        if relays:
            last_relays = relays
        parent_by_member = _parent_by_member_from_records(records, member_ids, leader_id=leader_id)
        leader_text = "".join(leader.log[log_start:])
        fresh_metrics = _latest_route_metrics(leader_text)
        if fresh_metrics is not None:
            last_metrics = fresh_metrics
        metrics = last_metrics
        route_ok = metrics is None or _route_metrics_ok(metrics, len(member_ids))
        bad_reasons: list[str] = []
        if len(online) != len(member_ids):
            missing = [member_id for member_id in member_ids if member_id not in online]
            bad_reasons.append(f"offline={missing}")
        if len(relays) != expected_relay_count:
            bad_reasons.append(f"relay_count={len(relays)} expected={expected_relay_count} relays={relays}")
        if expected_relay_ids is not None and relays != expected_relay_ids:
            bad_reasons.append(f"relay_ids={relays} expected={expected_relay_ids}")
        if expected_parent_by_member is not None and len(online) == len(member_ids):
            if parent_by_member != expected_parent_by_member:
                bad_reasons.append(f"parent_map={parent_by_member} expected={expected_parent_by_member}")
        if metrics is None:
            route_metrics_missing_polls += 1
            if not route_metrics_warned:
                _progress(f"{label}: route metrics missing; using member/relay evidence during soak")
                route_metrics_warned = True
        elif not route_ok:
            bad_reasons.append(f"route_metrics={metrics}")

        poll_count += 1
        if bad_reasons:
            bad_streak += 1
            sample = {
                "poll": poll_count,
                "elapsed_s": round(duration_s - max(0.0, end - time.time()), 1),
                "reasons": bad_reasons,
                "online": online,
                "relays": relays,
                "parent_by_member": parent_by_member,
                "route_metrics": metrics,
            }
            bad_samples.append(sample)
            _progress(f"{label}: bad poll {bad_streak}/{max_bad_polls}: {bad_reasons}")
            if bad_streak >= max_bad_polls:
                raise RuntimeError(f"{label} failed after consecutive bad polls: {bad_samples[-max_bad_polls:]}")
        else:
            bad_streak = 0

        if time.time() >= next_report:
            remaining = max(0.0, end - time.time())
            _progress(
                f"{label}: poll={poll_count} remaining={remaining:.0f}s "
                f"online={len(online)}/{len(member_ids)} relays={relays} metrics={metrics}"
            )
            next_report = time.time() + report_interval_s

        time.sleep(min(max(0.1, poll_s), max(0.1, end - time.time())))

    nr._assert_no_route_regressions_scoped(
        peers,
        leader_id,
        log_start_by_name=regression_log_start_by_name,
        expected_source_ids=member_ids,
    )
    return {
        "duration_s": duration_s,
        "poll_count": poll_count,
        "bad_samples": bad_samples,
        "route_metrics_missing_polls": route_metrics_missing_polls,
        "final_records": last_records,
        "final_relays": last_relays,
        "final_parent_by_member": _parent_by_member_from_records(last_records, member_ids, leader_id=leader_id),
        "final_route_metrics": _latest_route_metrics("".join(leader.log[log_start:])) or last_metrics,
    }


def _query_initial_statuses(peers: list[lc.Peer], expected_fw: str) -> dict[str, dict[str, object]]:
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
    leader_id: Optional[int] = None
    member_ids: list[int] = []
    log_dir = pathlib.Path(args.log_dir)
    summary_path = log_dir / "summary.json"
    summary: dict[str, object] = {
        "leader_port": args.leader_port,
        "member_ports": args.member_ports,
        "team_id": args.team_id,
        "channel": args.channel,
        "direct_cap": args.direct_cap,
        "relay_target": args.relay_target,
        "drop_method": args.drop_method,
        "drop_selection": args.drop_selection,
        "pre_drop_soak_s": args.pre_drop_soak_s,
        "post_rejoin_soak_s": args.post_rejoin_soak_s,
        "log_dir": str(log_dir),
    }
    try:
        if len(args.member_ports) < 3:
            raise RuntimeError("at least three member ports are required for double-drop validation")

        log_dir.mkdir(parents=True, exist_ok=True)
        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        members = [
            lc._open_peer(f"member{idx}", port, args.baudrate)
            for idx, port in enumerate(args.member_ports, start=1)
        ]
        peers = [leader, *members]

        _progress("drain serial boot logs")
        fb._drain_all(peers, args.initial_drain_s)
        statuses = _query_initial_statuses(peers, args.expected_fw)
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
        runtime_relay_override = int(leader_status.get("runtimeRelayTargetOverride") or 0)
        expected_relay_count = args.expected_relay_count
        if expected_relay_count is None:
            expected_relay_count = runtime_relay_override or int(leader_status.get("runtimeRelayTarget") or 0)
        if expected_relay_count <= 0:
            raise RuntimeError(
                "expected relay count is zero; pass --relay-target/--expected-relay-count for soak validation"
            )
        summary["leader_runtime_after_config"] = leader_status
        summary["runtime_direct_cap"] = runtime_direct_cap
        summary["expected_relay_count"] = expected_relay_count
        _progress(
            f"leader runtime: id={leader_id} suffix={leader_suffix} direct_cap={runtime_direct_cap} "
            f"relay_budget={leader_status.get('runtimeRelayBudget')} "
            f"relay_target={leader_status.get('runtimeRelayTarget')} "
            f"relay_override={leader_status.get('runtimeRelayTargetOverride')} "
            f"expected_relays={expected_relay_count}"
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
            stable_polls=max(2, min(args.stable_polls, 2)),
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            note="initial enrollment",
        )
        records = _wait_initial_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            relay_target=expected_relay_count,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=route_log_start,
            stable_polls=max(2, min(args.stable_polls, 2)),
        )
        topology = nr._topology_from_records(
            records,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[route_log_start:]),
        )
        summary["initial_records"] = records
        summary["initial_relays"] = relays
        summary["initial_topology"] = topology
        nr._assert_relays_are_leader_direct(topology, relay_ids=relays, leader_id=leader_id, stage="initial")
        _progress(f"enrollment PASS: relays={relays}; {_records_line(records, member_ids)}")
        _progress("initial topology:\n" + "\n".join(topology["tree_lines"]))
        initial_parent_by_member = _parent_by_member_from_records(records, member_ids, leader_id=leader_id)

        if args.pre_drop_soak_s > 0:
            pre_drop_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
            summary["pre_drop_soak"] = _wait_soak(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_ids,
                expected_relay_count=expected_relay_count,
                duration_s=args.pre_drop_soak_s,
                poll_s=args.soak_poll_s,
                report_interval_s=args.report_interval_s,
                max_bad_polls=args.max_bad_polls,
                log_start=route_log_start,
                regression_log_start_by_name=pre_drop_log_start_by_name,
                label="pre-drop",
                expected_parent_by_member=initial_parent_by_member,
                expected_relay_ids=relays,
            )

        drop_a_id, drop_b_id, drop_mode = _choose_double_drop(
            records,
            member_ids,
            topology,
            selection=args.drop_selection,
        )
        drop_ids = [drop_a_id, drop_b_id]
        drop_peers = [peer_by_id[drop_a_id], peer_by_id[drop_b_id]]
        remaining_ids = [member_id for member_id in member_ids if member_id not in drop_ids]
        remaining_peers = [leader, *[peer_by_id[member_id] for member_id in remaining_ids]]
        summary["double_drop"] = {
            "mode": drop_mode,
            "method": args.drop_method,
            "drop_ids": drop_ids,
            "drop_ports": [peer.port for peer in drop_peers],
        }
        _progress(
            "double signal-loss drop: "
            f"mode={drop_mode} method={args.drop_method} ids={drop_ids} ports={[peer.port for peer in drop_peers]}"
        )
        drop_log_start = len(leader.log)
        drop_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        drop_peer_boot_start_by_name: dict[str, int] = {}
        if args.drop_method == "leave":
            _send_leave_pair(drop_peers, peers, timeout_s=args.cmd_timeout_s)
        else:
            drop_peer_boot_start_by_name = _send_signal_loss_pair(drop_peers, args.reboot_command)
        offline_observed_ids: list[int] = []
        if args.drop_method == "leave":
            for drop_id in drop_ids:
                lc._wait_leader_offline_event(
                    leader,
                    peers,
                    member_id=drop_id,
                    timeout_s=args.offline_timeout_s,
                    note=f"leader offline after simultaneous {args.drop_method} id={drop_id}",
                    log_start=drop_log_start,
                )
                offline_observed_ids.append(drop_id)

            remaining_records = nr._wait_stable_remaining(
                leader,
                peers,
                leader_id=leader_id,
                remaining_ids=remaining_ids,
                left_ids=drop_ids,
                direct_cap=runtime_direct_cap,
                timeout_s=args.failover_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=drop_log_start,
                stable_polls=max(2, min(args.stable_polls, 3)),
                note="double-drop recovery",
            )
            remaining_records = _wait_relay_count(
                leader,
                peers,
                member_ids=remaining_ids,
                relay_target=expected_relay_count,
                timeout_s=args.failover_timeout_s,
                poll_s=args.poll_interval_s,
                note="double-drop remaining topology",
            )
            remaining_topology = nr._topology_from_records(
                remaining_records,
                member_ids=remaining_ids,
                leader_id=leader_id,
                leader_port=leader.port,
                peer_by_id=peer_by_id,
                leader_log_text="".join(leader.log[drop_log_start:]),
            )
            nr._assert_no_route_regressions_scoped(
                remaining_peers,
                leader_id,
                log_start_by_name=drop_log_start_by_name,
                expected_source_ids=remaining_ids,
            )
            remaining_relays = fb._relay_ids_from_records(remaining_records, remaining_ids)
            nr._assert_relays_are_leader_direct(
                remaining_topology,
                relay_ids=remaining_relays,
                leader_id=leader_id,
                stage="double-drop remaining",
            )
            summary["double_drop"].update(
                {
                    "offline_observed_ids": offline_observed_ids,
                    "remaining_records": remaining_records,
                    "remaining_relays": remaining_relays,
                    "remaining_topology": remaining_topology,
                }
            )
            _progress(
                "double-drop recovery PASS: "
                f"remaining_relays={summary['double_drop']['remaining_relays']}; "
                f"{_records_line(remaining_records, remaining_ids)}"
            )
        else:
            summary["double_drop"].update(
                {
                    "offline_observed_ids": offline_observed_ids,
                    "remaining_records": {},
                    "remaining_relays": [],
                    "remaining_topology": {},
                }
            )
            _progress(
                "double reboot: skip mandatory offline/remaining-topology gate; "
                "fast NV restore and leader-observed rejoin are validated next"
            )

        _progress(f"recover dropped members: method={args.drop_method} ids={drop_ids}")
        rejoin_start = len(leader.log)
        if args.drop_method == "leave":
            for peer in drop_peers:
                nr._rejoin_member(
                    peer,
                    peers,
                    leader_suffix=leader_suffix,
                    team_id=args.team_id,
                    channel=args.channel,
                    timeout_s=args.cmd_timeout_s,
                    boot_timeout_s=args.boot_timeout_s,
                )
            fb._wait_member_records(
                leader,
                peers,
                expected={member_id: (1, None) for member_id in drop_ids},
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                note="dropped members rejoined after explicit leave",
            )
            for peer in drop_peers:
                fb._wait_peer_joined(peer, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        else:
            fb._wait_pattern(
                drop_peers[0],
                peers,
                pattern=fb.MEMBER_RESTORE_PATTERN,
                timeout_s=args.boot_timeout_s,
                note=f"{drop_peers[0].name} restore from NV after signal-loss reboot",
                log_start=drop_peer_boot_start_by_name.get(drop_peers[0].name, 0),
            )
            fb._wait_pattern(
                drop_peers[1],
                peers,
                pattern=fb.MEMBER_RESTORE_PATTERN,
                timeout_s=args.boot_timeout_s,
                note=f"{drop_peers[1].name} restore from NV after signal-loss reboot",
                log_start=drop_peer_boot_start_by_name.get(drop_peers[1].name, 0),
            )
            _wait_dropped_pair_rejoined(
                leader,
                peers,
                drop_peers,
                leader_id=leader_id,
                drop_ids=drop_ids,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=drop_log_start,
            )
        final_records = _wait_initial_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            relay_target=expected_relay_count,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=rejoin_start,
            stable_polls=max(2, min(args.stable_polls, 2)),
        )
        rejoin_topology = nr._topology_from_records(
            final_records,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[rejoin_start:]),
        )
        post_rejoin_relays = fb._relay_ids_from_records(final_records, member_ids)
        nr._assert_relays_are_leader_direct(
            rejoin_topology,
            relay_ids=post_rejoin_relays,
            leader_id=leader_id,
            stage="post-rejoin",
        )
        summary["post_rejoin"] = {
            "records": final_records,
            "relays": post_rejoin_relays,
            "topology": rejoin_topology,
        }
        _progress(f"post-rejoin PASS: relays={summary['post_rejoin']['relays']}")
        post_rejoin_parent_by_member = _parent_by_member_from_records(final_records, member_ids, leader_id=leader_id)

        if args.post_rejoin_soak_s > 0:
            post_rejoin_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
            summary["post_rejoin_soak"] = _wait_soak(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_ids,
                expected_relay_count=expected_relay_count,
                duration_s=args.post_rejoin_soak_s,
                poll_s=args.soak_poll_s,
                report_interval_s=args.report_interval_s,
                max_bad_polls=args.max_bad_polls,
                log_start=rejoin_start,
                regression_log_start_by_name=post_rejoin_log_start_by_name,
                label="post-rejoin",
                expected_parent_by_member=post_rejoin_parent_by_member,
                expected_relay_ids=post_rejoin_relays,
            )

        summary["result"] = "PASS"
        _progress("PASS")
        return 0
    except Exception as exc:  # noqa: BLE001
        _progress(f"FAIL: {exc}")
        summary["result"] = "FAIL"
        summary["error"] = str(exc)
        crashes = _find_crash_events(peers)
        if crashes:
            _progress("crash evidence: " + "; ".join(crashes[:5]))
            summary["crashes"] = crashes[:20]
        if leader_id is not None:
            regressions = fb._find_route_regression_events(peers, leader_id, expected_source_ids=member_ids or None)
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
            crashes = _find_crash_events(peers)
            if crashes and "crashes" not in summary:
                _progress("crash evidence after log dump: " + "; ".join(crashes[:5]))
                summary["crashes"] = crashes[:20]
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
    parser = argparse.ArgumentParser(description="WS63 natural relay soak test with simultaneous double drop")
    parser.add_argument("--leader-port", required=True)
    parser.add_argument("--member-ports", nargs="+", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="v4.5.45-minimal")
    parser.add_argument("--team-id", type=int, default=7)
    parser.add_argument("--channel", type=int, default=33)
    parser.add_argument("--direct-cap", type=int, default=4)
    parser.add_argument("--relay-target", type=int, default=4)
    parser.add_argument("--expected-relay-count", type=int, default=None)
    parser.add_argument("--skip-direct-config", action="store_true")
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument(
        "--drop-method",
        choices=("reboot", "leave"),
        default="reboot",
        help="default reboot simulates signal loss; leave is explicit compatibility mode",
    )
    parser.add_argument(
        "--drop-selection",
        choices=("non-relay-pair", "relay-plus-child", "relay-plus-member", "auto-legacy"),
        default="non-relay-pair",
        help="default validates two ordinary member losses; relay selections are explicit pressure modes",
    )
    parser.add_argument("--no-clean-start", action="store_true")
    parser.add_argument("--initial-drain-s", type=float, default=2.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=20.0)
    parser.add_argument("--boot-timeout-s", type=float, default=85.0)
    parser.add_argument("--discovery-timeout-s", type=float, default=180.0)
    parser.add_argument("--state-timeout-s", type=float, default=140.0)
    parser.add_argument("--route-timeout-s", type=float, default=180.0)
    parser.add_argument("--offline-timeout-s", type=float, default=60.0)
    parser.add_argument("--failover-timeout-s", type=float, default=180.0)
    parser.add_argument("--stable-polls", type=int, default=4)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--pre-drop-soak-s", type=float, default=300.0)
    parser.add_argument("--post-rejoin-soak-s", type=float, default=3300.0)
    parser.add_argument("--soak-poll-s", type=float, default=5.0)
    parser.add_argument("--report-interval-s", type=float, default=60.0)
    parser.add_argument("--max-bad-polls", type=int, default=3)
    parser.add_argument("--log-dir", required=True)
    return parser


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
