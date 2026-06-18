#!/usr/bin/env python3
"""WS63 natural relay signal-loss/rejoin recovery test.

This live-hardware test intentionally does not approve or name any relay.
It enrolls all members naturally, lets firmware policy elect relay nodes, then
tests signal-loss/rejoin for a non-relay member. The default drop method is a
soft reboot, which models the leader losing the member radio without an active
team leave. Explicit leave and relay-drop scenarios are retained only as
compatibility/pressure modes.
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
except ModuleNotFoundError:  # pragma: no cover - direct script execution
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(repo_root))
    from automation.ws63.tools import ws63_four_board_relay_test as fb
    from automation.ws63.tools import ws63_link_cycle_test as lc


def _progress(message: str) -> None:
    print(f"[natural-leave] {message}", flush=True)


def _records_line(records: fb.MemberRecords, member_ids: Iterable[int]) -> str:
    parts: list[str] = []
    for member_id in member_ids:
        record = records.get(member_id, {})
        parts.append(
            f"{member_id}:online={record.get('online')} relay={record.get('relay')} "
            f"tier={record.get('tier')} last={record.get('last_seen')}"
        )
    return "; ".join(parts)


def _parse_pending_ids(text: str) -> list[int]:
    ids: list[int] = []
    for match in re.finditer(r"\bpending member=(\d+)\b", text):
        member_id = int(match.group(1))
        if member_id not in ids:
            ids.append(member_id)
    return ids


_ROUTE_NOTE_RE = re.compile(r"\broute note member=(\d+)\b.*?\bconn=(\d+)\b.*?\bnext=(\d+)\b")
_ROUTE_CLEAR_CONN_RE = re.compile(r"\broute clear conn=(\d+)\b")
_ROUTE_CLEAR_NEXT_RE = re.compile(r"\broute clear next_hop=(\d+)\b")


def _latest_route_next_by_member(text: str) -> dict[int, int]:
    latest: dict[int, tuple[int, int]] = {}
    for line in text.splitlines():
        note = _ROUTE_NOTE_RE.search(line)
        if note is not None:
            latest[int(note.group(1))] = (int(note.group(3)), int(note.group(2)))
            continue
        clear_conn = _ROUTE_CLEAR_CONN_RE.search(line)
        if clear_conn is not None:
            conn_id = int(clear_conn.group(1))
            latest = {member_id: route for member_id, route in latest.items() if route[1] != conn_id}
            continue
        clear_next = _ROUTE_CLEAR_NEXT_RE.search(line)
        if clear_next is not None:
            next_hop = int(clear_next.group(1))
            latest = {member_id: route for member_id, route in latest.items() if route[0] != next_hop}
    return {member_id: route[0] for member_id, route in latest.items()}


def _topology_from_records(
    records: fb.MemberRecords,
    *,
    member_ids: list[int],
    leader_id: int,
    leader_port: str,
    peer_by_id: dict[int, lc.Peer],
    leader_log_text: str,
) -> dict[str, object]:
    route_next = _latest_route_next_by_member(leader_log_text)
    children_by_parent: dict[int, list[int]] = {leader_id: []}
    parent_by_member: dict[int, int] = {}
    for member_id in member_ids:
        record = records.get(member_id)
        if record is None or record.get("online") != 1:
            continue
        next_hop = route_next.get(member_id)
        if next_hop is None:
            next_hop = record.get("next_hop")
        parent_record = records.get(next_hop)
        if (
            next_hop in member_ids
            and next_hop != member_id
            and parent_record is not None
            and parent_record.get("online") == 1
            and parent_record.get("relay") == 1
        ):
            parent_id = next_hop
        else:
            record_parent = record.get("parent_id")
            if (
                record_parent in member_ids
                and record_parent != member_id
                and records.get(record_parent, {}).get("online") == 1
                and records.get(record_parent, {}).get("relay") == 1
            ):
                parent_id = int(record_parent)
            else:
                parent_id = leader_id
        parent_by_member[member_id] = parent_id
        children_by_parent.setdefault(parent_id, []).append(member_id)
        children_by_parent.setdefault(member_id, [])

    for children in children_by_parent.values():
        children.sort()

    def label(node_id: int) -> str:
        if node_id == leader_id:
            return f"L{leader_id}({leader_port})"
        peer = peer_by_id.get(node_id)
        port = peer.port if peer is not None else "?"
        record = records.get(node_id, {})
        relay = record.get("relay", "?")
        tier = record.get("tier", "?")
        return f"M{node_id}({port},relay={relay},tier={tier})"

    lines: list[str] = [label(leader_id)]

    def walk(parent_id: int, prefix: str = "") -> None:
        children = children_by_parent.get(parent_id, [])
        for idx, child_id in enumerate(children):
            last = idx == len(children) - 1
            branch = "`- " if last else "|- "
            lines.append(f"{prefix}{branch}{label(child_id)}")
            walk(child_id, prefix + ("   " if last else "|  "))

    walk(leader_id)
    return {
        "leader_id": leader_id,
        "leader_port": leader_port,
        "parent_by_member": {str(member_id): parent_id for member_id, parent_id in parent_by_member.items()},
        "route_next_by_member": {str(member_id): next_hop for member_id, next_hop in route_next.items()},
        "tree_lines": lines,
    }


def _topology_int_lookup(mapping: object, member_id: int) -> Optional[int]:
    if not isinstance(mapping, dict):
        return None
    value = mapping.get(str(member_id))
    if value is None:
        value = mapping.get(member_id)
    try:
        return None if value is None else int(value)
    except (TypeError, ValueError):
        return None


def _assert_relays_are_leader_direct(
    topology: dict[str, object],
    *,
    relay_ids: list[int],
    leader_id: int,
    stage: str,
) -> None:
    parent_by_member = topology.get("parent_by_member", {})
    route_next_by_member = topology.get("route_next_by_member", {})
    tree_lines = topology.get("tree_lines", [])
    violations: list[str] = []

    for relay_id in relay_ids:
        parent_id = _topology_int_lookup(parent_by_member, relay_id)
        route_next_id = _topology_int_lookup(route_next_by_member, relay_id)
        if parent_id is None:
            violations.append(f"relay={relay_id} parent=missing leader={leader_id}")
        elif parent_id != leader_id:
            violations.append(f"relay={relay_id} parent={parent_id} leader={leader_id}")
        if route_next_id is not None and route_next_id != leader_id:
            violations.append(f"relay={relay_id} route_next={route_next_id} leader={leader_id}")
        for line in tree_lines:
            tier_match = re.search(r"\btier=([0-9]+)", line)
            if f"M{relay_id}(" in line and "relay=1" in line and tier_match and int(tier_match.group(1)) > 1:
                violations.append(f"relay={relay_id} tier={tier_match.group(1)}")
                break

    if violations:
        raise RuntimeError(
            f"{stage} relay leader-direct invariant failed: "
            + "; ".join(violations)
            + "; topology="
            + json.dumps(topology, ensure_ascii=False, default=str)
        )


def _assert_old_relay_rejoins_as_child_or_member(
    topology: dict[str, object],
    records: fb.MemberRecords,
    *,
    old_relay_id: int,
    leader_id: int,
    current_relays: list[int],
    stage: str,
) -> None:
    record = records.get(old_relay_id)
    if record is None:
        raise RuntimeError(f"{stage} old relay missing from records: member={old_relay_id}")
    if record.get("online") != 1:
        raise RuntimeError(f"{stage} old relay not online after rejoin: member={old_relay_id} record={record}")
    if old_relay_id in current_relays or record.get("relay") == 1:
        raise RuntimeError(
            f"{stage} old relay reclaimed relay role unexpectedly: "
            f"member={old_relay_id} relays={current_relays} record={record}"
        )

    parent_by_member = topology.get("parent_by_member", {})
    parent_id = _topology_int_lookup(parent_by_member, old_relay_id)
    if parent_id is None:
        raise RuntimeError(f"{stage} old relay parent missing from topology: member={old_relay_id} topology={topology}")

    if current_relays:
        if parent_id == leader_id:
            raise RuntimeError(
                f"{stage} old relay rejoined direct to leader while replacement relay exists: "
                f"member={old_relay_id} parent={parent_id} relays={current_relays} topology={topology}"
            )
        if parent_id not in current_relays:
            raise RuntimeError(
                f"{stage} old relay parent is not a current relay: "
                f"member={old_relay_id} parent={parent_id} relays={current_relays} topology={topology}"
            )


def _wait_discovery_window(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    member_ids: list[int],
    leader_id: int,
    timeout_s: float,
    poll_s: float,
) -> dict[str, object]:
    _progress(f"discovery window {timeout_s:.0f}s: collect pending/hello/online members before pairing stop")
    start = len(leader.log)
    end = time.time() + timeout_s
    seen_ids: set[int] = set()
    pending_ids: set[int] = set()
    online_ids: set[int] = set()
    last_records: fb.MemberRecords = {}
    stable_all_seen = 0
    next_report = time.time()

    while time.time() < end:
        records = fb._query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last_records = records
        for member_id in member_ids:
            if records.get(member_id, {}).get("online") == 1:
                online_ids.add(member_id)
                seen_ids.add(member_id)

        pending_text = fb._send_and_collect(leader, peers, "pairing pending", min(0.6, poll_s), clear_before=False)
        pending_ids.update(member_id for member_id in _parse_pending_ids(pending_text) if member_id in member_ids)
        leader_text = "".join(leader.log[start:])
        for member_id in member_ids:
            if re.search(rf"\bpending member={member_id}\b", leader_text):
                pending_ids.add(member_id)
                seen_ids.add(member_id)
            if re.search(rf"\bHELLO\s+{member_id}->{leader_id}\b", leader_text):
                seen_ids.add(member_id)
            if re.search(rf"\bmember={member_id}\b", leader_text):
                seen_ids.add(member_id)

        seen_ids.update(pending_ids)
        missing_ids = [member_id for member_id in member_ids if member_id not in seen_ids]
        if time.time() >= next_report:
            _progress(
                "discovery: "
                f"seen={sorted(seen_ids)} online={sorted(online_ids)} "
                f"pending={sorted(pending_ids)} missing={missing_ids}"
            )
            next_report = time.time() + 5.0

        if not missing_ids:
            stable_all_seen += 1
            if stable_all_seen >= 2:
                break
        else:
            stable_all_seen = 0
        time.sleep(max(0.1, poll_s - 1.2))

    missing_ids = [member_id for member_id in member_ids if member_id not in seen_ids]
    return {
        "seen_ids": sorted(seen_ids),
        "online_ids": sorted(online_ids),
        "pending_ids": sorted(pending_ids),
        "missing_ids": missing_ids,
        "last_records": last_records,
    }


def _relay_coverage(
    records: fb.MemberRecords,
    member_ids: list[int],
    direct_cap: int,
    peer_by_id: dict[int, lc.Peer],
) -> dict[str, object]:
    online_ids = [member_id for member_id in member_ids if records.get(member_id, {}).get("online") == 1]
    offline_ids = [member_id for member_id in member_ids if member_id not in online_ids]
    effective_direct_cap = max(1, direct_cap)
    return {
        "online_member_ids": online_ids,
        "offline_member_ids": offline_ids,
        "offline_member_ports": [peer_by_id[member_id].port for member_id in offline_ids if member_id in peer_by_id],
        "online_member_count": len(online_ids),
        "expected_member_count": len(member_ids),
        "direct_cap": effective_direct_cap,
        "required_online_for_relay": effective_direct_cap + 1,
        "relay_covered": len(online_ids) > effective_direct_cap,
    }


def _mark_relay_not_covered(
    summary: dict[str, object],
    *,
    stage: str,
    coverage: dict[str, object],
) -> None:
    summary["relay_scenario"] = "NOT_COVERED"
    summary["relay_not_covered_stage"] = stage
    summary["relay_not_covered_reason"] = (
        "online member count is not greater than direct_cap, so firmware never entered relay election"
    )
    summary["relay_coverage"] = coverage


def _wait_online_set(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    online_ids: list[int],
    offline_ids: list[int],
    timeout_s: float,
    poll_s: float,
    note: str,
    require_offline_ids: bool = True,
) -> fb.MemberRecords:
    expected: dict[int, tuple[Optional[int], Optional[int]]] = {
        member_id: (1, None) for member_id in online_ids
    }
    if require_offline_ids:
        for member_id in offline_ids:
            expected[member_id] = (0, None)

    end = time.time() + timeout_s
    last: fb.MemberRecords = {}
    while time.time() < end:
        records = fb._query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last = records
        ok = True
        for member_id, (online, relay) in expected.items():
            record = records.get(member_id)
            if online == 0 and record is None:
                continue
            if record is None:
                ok = False
                break
            if online is not None and record.get("online") != online:
                ok = False
                break
            if relay is not None and record.get("relay") != relay:
                ok = False
                break
        if ok:
            return records
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"timeout waiting for {note}: last={last}")


def _wait_stable_remaining(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    leader_id: int,
    remaining_ids: list[int],
    left_ids: list[int],
    direct_cap: int,
    timeout_s: float,
    poll_s: float,
    log_start: int,
    stable_polls: int,
    note: str,
    require_left_ids_offline: bool = True,
) -> fb.MemberRecords:
    records = _wait_online_set(
        leader,
        peers,
        online_ids=remaining_ids,
        offline_ids=left_ids,
        timeout_s=timeout_s,
        poll_s=poll_s,
        note=f"{note}: online/offline member table",
        require_offline_ids=require_left_ids_offline,
    )
    try:
        records = fb._wait_stable_final_topology(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=remaining_ids,
            direct_cap=direct_cap,
            timeout_s=timeout_s,
            poll_s=poll_s,
            log_start=log_start,
            stable_polls=stable_polls,
            allow_any_converged=True,
        )
    except RuntimeError as exc:
        _progress(f"{note}: topology metrics not fully refreshed, keeping member-table evidence: {exc}")
    return records


def _wait_topology_ready(
    leader: lc.Peer,
    peers: list[lc.Peer],
    *,
    leader_id: int,
    member_ids: list[int],
    direct_cap: int,
    timeout_s: float,
    poll_s: float,
    log_start: int,
    stable_polls: int,
    note: str,
) -> fb.MemberRecords:
    records = fb._wait_member_records(
        leader,
        peers,
        expected={member_id: (1, None) for member_id in member_ids},
        timeout_s=timeout_s,
        poll_s=poll_s,
        note=f"{note}: all expected members online",
    )
    try:
        records = fb._wait_stable_final_topology(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=direct_cap,
            timeout_s=timeout_s,
            poll_s=poll_s,
            log_start=log_start,
            stable_polls=stable_polls,
            allow_any_converged=True,
        )
    except RuntimeError as exc:
        _progress(f"{note}: topology metrics not fully refreshed, keeping member-table evidence: {exc}")
    return records


def _snapshot_peer_join_state(
    peer: lc.Peer,
    peers: list[lc.Peer],
    *,
    note: str,
) -> str:
    last = fb._send_and_collect(peer, peers, "state", 0.6)
    if re.search(r"\bjoined=1\b", last):
        _progress(f"{note}: peer-local joined=1")
        return last
    _progress(f"{note}: peer-local joined not confirmed from one-shot snapshot; leader records stay authoritative, last={last!r}")
    return last


def _send_leave(
    peer: lc.Peer,
    peers: list[lc.Peer],
    *,
    timeout_s: float,
    note: str,
) -> None:
    fb._send_cfg_and_wait(
        peer,
        peers,
        command="leave",
        pattern=r"leave ret=0",
        timeout_s=timeout_s,
        note=note,
    )


def _drop_peer(
    peer: lc.Peer,
    peers: list[lc.Peer],
    *,
    method: str,
    reboot_command: str,
    timeout_s: float,
    note: str,
) -> int:
    log_start = len(peer.log)
    if method == "leave":
        _send_leave(peer, peers, timeout_s=timeout_s, note=note)
        return log_start
    if method == "reboot":
        fb._send_cli_line(peer, reboot_command)
        return log_start
    raise ValueError(f"unsupported drop method: {method}")


def _wait_reboot_restore(
    peer: lc.Peer,
    peers: list[lc.Peer],
    *,
    log_start: int,
    timeout_s: float,
    note: str,
) -> None:
    fb._wait_pattern(
        peer,
        peers,
        pattern=fb.MEMBER_RESTORE_PATTERN,
        timeout_s=timeout_s,
        note=note,
        log_start=log_start,
    )


def _recover_dropped_member(
    member: lc.Peer,
    peers: list[lc.Peer],
    *,
    method: str,
    reboot_log_start: int,
    leader_suffix: str,
    team_id: int,
    channel: int,
    timeout_s: float,
    boot_timeout_s: float,
) -> None:
    if method == "leave":
        _rejoin_member(
            member,
            peers,
            leader_suffix=leader_suffix,
            team_id=team_id,
            channel=channel,
            timeout_s=timeout_s,
            boot_timeout_s=boot_timeout_s,
        )
        return
    _wait_reboot_restore(
        member,
        peers,
        log_start=reboot_log_start,
        timeout_s=boot_timeout_s,
        note=f"{member.name} restore from NV after signal-loss reboot",
    )


def _assert_no_route_regressions_scoped(
    peers: Iterable[lc.Peer],
    leader_id: int,
    *,
    log_start_by_name: dict[str, int] | None = None,
    expected_source_ids: Iterable[int] | None = None,
) -> None:
    scoped_peers: list[lc.Peer] = []
    for peer in peers:
        log_start = 0
        if log_start_by_name is not None:
            log_start = log_start_by_name.get(peer.name, 0)
        scoped_peers.append(
            lc.Peer(
                name=peer.name,
                port=peer.port,
                baudrate=peer.baudrate,
                ser=peer.ser,
                log=peer.log[log_start:],
            )
        )
    fb._assert_no_route_regressions(scoped_peers, leader_id, expected_source_ids=expected_source_ids)


def _rejoin_member(
    member: lc.Peer,
    peers: list[lc.Peer],
    *,
    leader_suffix: str,
    team_id: int,
    channel: int,
    timeout_s: float,
    boot_timeout_s: float,
) -> None:
    fb._configure_member_role(
        member,
        peers,
        leader_suffix=leader_suffix,
        team_id=team_id,
        channel=channel,
        timeout_s=timeout_s,
        boot_timeout_s=boot_timeout_s,
    )


def _wait_natural_enrollment(
    leader: lc.Peer,
    members: list[lc.Peer],
    peers: list[lc.Peer],
    *,
    leader_id: int,
    member_ids: list[int],
    direct_cap: int,
    timeout_s: float,
    poll_s: float,
    log_start: int,
    stable_polls: int,
    leader_port: str,
    peer_by_id: dict[int, lc.Peer],
    note: str,
) -> tuple[fb.MemberRecords, list[int], dict[str, object]]:
    _progress(f"{note}: allow=all natural enrollment without pairing window")
    records = _wait_topology_ready(
        leader,
        peers,
        leader_id=leader_id,
        member_ids=member_ids,
        direct_cap=direct_cap,
        timeout_s=timeout_s,
        poll_s=poll_s,
        log_start=log_start,
        stable_polls=stable_polls,
        note=note,
    )
    for member in members:
        _snapshot_peer_join_state(
            member,
            peers,
            note=f"{note} {member.name}",
        )
    relays = fb._relay_ids_from_records(records, member_ids)
    topology = _topology_from_records(
        records,
        member_ids=member_ids,
        leader_id=leader_id,
        leader_port=leader_port,
        peer_by_id=peer_by_id,
        leader_log_text="".join(leader.log[log_start:]),
    )
    return records, relays, topology


def run(args: argparse.Namespace) -> int:
    peers: list[lc.Peer] = []
    leader_id: Optional[int] = None
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
        "drop_method": args.drop_method,
        "include_relay_drop": args.include_relay_drop,
        "log_dir": str(log_dir),
    }
    try:
        if len(args.member_ports) < 2:
            raise RuntimeError("at least two member ports are required to observe relay behavior")

        log_dir.mkdir(parents=True, exist_ok=True)
        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        members = [
            lc._open_peer(f"member{idx}", port, args.baudrate)
            for idx, port in enumerate(args.member_ports, start=1)
        ]
        peers = [leader, *members]

        _progress("drain serial boot logs")
        fb._drain_all(peers, args.initial_drain_s)

        statuses = {}
        for peer in peers:
            status = fb._query_cfg(peer, peers, window_s=3.0, attempts=5)
            fw = status.get("fw")
            if args.expected_fw and fw != args.expected_fw:
                raise RuntimeError(f"{peer.name} firmware mismatch: expected {args.expected_fw}, got {fw}")
            statuses[peer.name] = status
            _progress(
                f"{peer.name} {peer.port}: fw={fw} suffix={status.get('selfSuffix')} "
                f"route={status.get('routeId')} role={status.get('runtimeRole')}"
            )
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
        summary["leader_runtime_after_config"] = leader_status
        _progress(
            f"leader runtime: id={leader_id} suffix={leader_suffix} "
            f"direct_cap={runtime_direct_cap} relay_budget={leader_status.get('runtimeRelayBudget')} "
            f"relay_target={leader_status.get('runtimeRelayTarget')} "
            f"relay_override={leader_status.get('runtimeRelayTargetOverride')}"
        )

        route_log_start = len(leader.log)
        summary["enrollment_mode"] = "allow_all_no_pairing_window"
        try:
            records, relays, topology = _wait_natural_enrollment(
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
        except RuntimeError as exc:
            records = fb._query_records_once(leader, peers, min(0.6, args.poll_interval_s))
            coverage = _relay_coverage(records, member_ids, runtime_direct_cap, peer_by_id)
            summary["enrollment_records"] = records
            summary["failure_cause"] = "member_upstream_not_ready"
            if coverage["relay_covered"] is False:
                _mark_relay_not_covered(summary, stage="enrollment", coverage=coverage)
            raise RuntimeError(
                "member enrollment incomplete under allow=all natural policy; "
                "relay election is not a valid failure signal until enough members are online: "
                f"{coverage}"
            ) from exc
        summary["initial_records"] = records
        summary["initial_relays"] = relays
        summary["initial_topology"] = topology
        _assert_relays_are_leader_direct(topology, relay_ids=relays, leader_id=leader_id, stage="initial")
        _progress(f"natural enrollment PASS: relays={relays}; {_records_line(records, member_ids)}")
        _progress("initial topology:\n" + "\n".join(topology["tree_lines"]))
        if not relays:
            coverage = _relay_coverage(records, member_ids, runtime_direct_cap, peer_by_id)
            if coverage["relay_covered"] is False:
                _mark_relay_not_covered(summary, stage="initial_enrollment", coverage=coverage)
                summary["result"] = "SKIP"
                _progress(
                    "SKIP: relay scenario not covered; "
                    f"online_members={coverage['online_member_count']}/"
                    f"{coverage['expected_member_count']} direct_cap={coverage['direct_cap']}"
                )
                return 0
            raise RuntimeError(f"relay election not observed despite sufficient online members: {coverage}")

        non_relay_id = fb._prefer_non_relay_member_id(records, member_ids)
        if non_relay_id is None:
            raise RuntimeError(f"no non-relay member available for leave test: {records}")
        non_relay = peer_by_id[non_relay_id]
        _progress(
            f"member drop test: {args.drop_method} non-relay "
            f"{non_relay.name} {non_relay.port} id={non_relay_id}"
        )
        member_drop_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        member_drop_start = len(leader.log)
        member_boot_start = _drop_peer(
            non_relay,
            peers,
            method=args.drop_method,
            reboot_command=args.reboot_command,
            timeout_s=args.cmd_timeout_s,
            note=f"{non_relay.name} {args.drop_method}",
        )
        member_drop_offline_observed = False
        if args.drop_method == "leave":
            lc._wait_leader_offline_event(
                leader,
                peers,
                member_id=non_relay_id,
                timeout_s=args.offline_timeout_s,
                note=f"leader offline after non-relay {args.drop_method}",
                log_start=member_drop_start,
            )
            member_drop_offline_observed = True
        else:
            _progress(
                "member reboot: skip mandatory offline gate; "
                "fast NV restore and leader-observed rejoin are validated next"
            )
        remaining_after_member_leave = [member_id for member_id in member_ids if member_id != non_relay_id]
        remaining_peers_after_member_leave = [
            leader,
            *[peer_by_id[member_id] for member_id in remaining_after_member_leave],
        ]
        member_leave_records: fb.MemberRecords = {}
        if args.drop_method == "leave":
            member_leave_records = _wait_stable_remaining(
                leader,
                peers,
                leader_id=leader_id,
                remaining_ids=remaining_after_member_leave,
                left_ids=[non_relay_id],
                direct_cap=runtime_direct_cap,
                timeout_s=args.route_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=member_drop_start,
                stable_polls=max(2, min(args.stable_polls, 3)),
                note="member leave recovery",
            )
        else:
            try:
                member_leave_records = fb._wait_member_records(
                    leader,
                    peers,
                    expected={member_id: (1, None) for member_id in remaining_after_member_leave},
                    timeout_s=min(args.route_timeout_s, args.offline_timeout_s),
                    poll_s=args.poll_interval_s,
                    note="remaining members online after signal-loss reboot",
                )
            except RuntimeError as exc:
                _progress(f"remaining topology evidence skipped while rebooting member: {exc}")
        member_leave_topology = _topology_from_records(
            member_leave_records,
            member_ids=remaining_after_member_leave,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[member_drop_start:]),
        )
        _assert_no_route_regressions_scoped(
            remaining_peers_after_member_leave,
            leader_id,
            log_start_by_name=member_drop_log_start_by_name,
            expected_source_ids=remaining_after_member_leave,
        )
        summary["member_leave"] = {
            "method": args.drop_method,
            "member_id": non_relay_id,
            "port": non_relay.port,
            "offline_observed": member_drop_offline_observed,
            "records": member_leave_records,
            "relays": fb._relay_ids_from_records(member_leave_records, remaining_after_member_leave),
            "topology": member_leave_topology,
        }
        if member_drop_offline_observed:
            _progress(
                f"member {args.drop_method} offline observed: remaining online; "
                f"{_records_line(member_leave_records, remaining_after_member_leave)}"
            )
        else:
            _progress(
                f"member {args.drop_method} fast-rejoin path: remaining members checked; "
                f"{_records_line(member_leave_records, remaining_after_member_leave)}"
            )
        if member_leave_topology["tree_lines"]:
            _progress("member drop topology:\n" + "\n".join(member_leave_topology["tree_lines"]))

        _progress(f"member recover: {non_relay.name} {non_relay.port} id={non_relay_id}")
        member_rejoin_start = len(leader.log)
        _recover_dropped_member(
            non_relay,
            peers,
            method=args.drop_method,
            reboot_log_start=member_boot_start,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        if args.drop_method == "reboot":
            fb._wait_leader_observes_member_reboot_rejoin(
                leader,
                peers,
                member_id=non_relay_id,
                leader_id=leader_id,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=member_drop_start,
            )
        fb._wait_member_records(
            leader,
            peers,
            expected={non_relay_id: (1, None)},
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note=f"non-relay recovered after {args.drop_method}",
        )
        _snapshot_peer_join_state(
            non_relay,
            peers,
            note=f"non-relay recovered {non_relay.name}",
        )
        records_after_member_rejoin = _wait_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=member_rejoin_start,
            stable_polls=max(2, min(args.stable_polls, 3)),
            note="member rejoin",
        )
        relays_after_member_rejoin = fb._relay_ids_from_records(records_after_member_rejoin, member_ids)
        member_rejoin_topology = _topology_from_records(
            records_after_member_rejoin,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[member_rejoin_start:]),
        )
        summary["member_rejoin"] = {
            "member_id": non_relay_id,
            "records": records_after_member_rejoin,
            "relays": relays_after_member_rejoin,
            "topology": member_rejoin_topology,
        }
        _progress(f"member recovery PASS: relays={relays_after_member_rejoin}")
        _progress("member recovery topology:\n" + "\n".join(member_rejoin_topology["tree_lines"]))

        if not args.include_relay_drop:
            summary["result"] = "PASS"
            _progress("PASS")
            return 0

        if not relays_after_member_rejoin:
            coverage = _relay_coverage(records_after_member_rejoin, member_ids, runtime_direct_cap, peer_by_id)
            if coverage["relay_covered"] is False:
                _mark_relay_not_covered(summary, stage="after_member_rejoin", coverage=coverage)
                summary["result"] = "SKIP"
                _progress(
                    "SKIP: relay leave/rejoin scenario not covered; "
                    f"online_members={coverage['online_member_count']}/"
                    f"{coverage['expected_member_count']} direct_cap={coverage['direct_cap']}"
                )
                return 0
            raise RuntimeError(f"no relay available for relay leave despite sufficient online members: {coverage}")
        relay_id = relays_after_member_rejoin[0]
        relay_peer = peer_by_id[relay_id]
        _progress(
            f"relay drop test: {args.drop_method} actual relay "
            f"{relay_peer.name} {relay_peer.port} id={relay_id}"
        )
        relay_drop_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        relay_drop_start = len(leader.log)
        relay_boot_start = _drop_peer(
            relay_peer,
            peers,
            method=args.drop_method,
            reboot_command=args.reboot_command,
            timeout_s=args.cmd_timeout_s,
            note=f"{relay_peer.name} {args.drop_method}",
        )
        lc._wait_leader_offline_event(
            leader,
            peers,
            member_id=relay_id,
            timeout_s=args.offline_timeout_s,
            note=f"leader offline after relay {args.drop_method}",
            log_start=relay_drop_start,
        )
        remaining_after_relay_leave = [member_id for member_id in member_ids if member_id != relay_id]
        remaining_peers_after_relay_leave = [
            leader,
            *[peer_by_id[member_id] for member_id in remaining_after_relay_leave],
        ]
        relay_leave_records = _wait_stable_remaining(
            leader,
            peers,
            leader_id=leader_id,
            remaining_ids=remaining_after_relay_leave,
            left_ids=[relay_id],
            direct_cap=runtime_direct_cap,
            timeout_s=args.failover_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=relay_drop_start,
            stable_polls=max(2, min(args.stable_polls, 3)),
            note="relay leave recovery",
            require_left_ids_offline=(args.drop_method == "leave"),
        )
        relays_after_relay_leave = fb._relay_ids_from_records(relay_leave_records, remaining_after_relay_leave)
        relay_leave_topology = _topology_from_records(
            relay_leave_records,
            member_ids=remaining_after_relay_leave,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[relay_drop_start:]),
        )
        if len(remaining_after_relay_leave) > runtime_direct_cap and not relays_after_relay_leave:
            raise RuntimeError(f"relay leave did not elect a replacement relay: {relay_leave_records}")
        _assert_no_route_regressions_scoped(
            remaining_peers_after_relay_leave,
            leader_id,
            log_start_by_name=relay_drop_log_start_by_name,
            expected_source_ids=remaining_after_relay_leave,
        )
        summary["relay_leave"] = {
            "method": args.drop_method,
            "relay_id": relay_id,
            "port": relay_peer.port,
            "records": relay_leave_records,
            "relays": relays_after_relay_leave,
            "topology": relay_leave_topology,
        }
        _progress(
            f"relay leave PASS: replacement_relays={relays_after_relay_leave}; "
            f"{_records_line(relay_leave_records, remaining_after_relay_leave)}"
        )
        _progress("relay leave topology:\n" + "\n".join(relay_leave_topology["tree_lines"]))

        _progress(f"relay recover: {relay_peer.name} {relay_peer.port} id={relay_id}")
        relay_rejoin_start = len(leader.log)
        _recover_dropped_member(
            relay_peer,
            peers,
            method=args.drop_method,
            reboot_log_start=relay_boot_start,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        if args.drop_method == "reboot":
            fb._wait_leader_observes_member_reboot_rejoin(
                leader,
                peers,
                member_id=relay_id,
                leader_id=leader_id,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=relay_drop_start,
            )
        fb._wait_member_records(
            leader,
            peers,
            expected={relay_id: (1, None)},
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note=f"relay member recovered after {args.drop_method}",
        )
        _snapshot_peer_join_state(
            relay_peer,
            peers,
            note=f"relay recovered {relay_peer.name}",
        )
        relay_rejoin_log_start_by_name = {peer.name: len(peer.log) for peer in peers}
        final_records = _wait_topology_ready(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=member_ids,
            direct_cap=runtime_direct_cap,
            timeout_s=args.route_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=relay_rejoin_start,
            stable_polls=max(2, min(args.stable_polls, 3)),
            note="relay rejoin",
        )
        final_relays = fb._relay_ids_from_records(final_records, member_ids)
        final_topology = _topology_from_records(
            final_records,
            member_ids=member_ids,
            leader_id=leader_id,
            leader_port=leader.port,
            peer_by_id=peer_by_id,
            leader_log_text="".join(leader.log[relay_rejoin_start:]),
        )
        _assert_no_route_regressions_scoped(
            peers,
            leader_id,
            log_start_by_name=relay_rejoin_log_start_by_name,
            expected_source_ids=member_ids,
        )
        _assert_old_relay_rejoins_as_child_or_member(
            final_topology,
            final_records,
            old_relay_id=relay_id,
            leader_id=leader_id,
            current_relays=final_relays,
            stage="relay rejoin",
        )
        summary["relay_rejoin"] = {
            "relay_id": relay_id,
            "records": final_records,
            "relays": final_relays,
            "topology": final_topology,
        }
        summary["result"] = "PASS"
        _progress(f"relay rejoin PASS: final_relays={final_relays}; {_records_line(final_records, member_ids)}")
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
    parser = argparse.ArgumentParser(description="WS63 live natural relay leave/rejoin recovery test")
    parser.add_argument("--leader-port", required=True)
    parser.add_argument("--member-ports", nargs="+", required=True)
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="v4.5.64-minimal")
    parser.add_argument("--team-id", type=int, default=7)
    parser.add_argument("--channel", type=int, default=33)
    parser.add_argument("--direct-cap", type=int, default=4)
    parser.add_argument("--relay-target", type=int, default=None, help="leader relay target override; 0 restores automatic target")
    parser.add_argument("--skip-direct-config", action="store_true")
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument(
        "--drop-method",
        choices=("reboot", "leave"),
        default="reboot",
        help="default reboot simulates signal loss; leave is explicit compatibility mode",
    )
    parser.add_argument(
        "--include-relay-drop",
        action="store_true",
        help="also test relay loss; this is future stable-regrouping pressure, not the default acceptance path",
    )
    parser.add_argument("--no-clean-start", action="store_true")
    parser.add_argument("--initial-drain-s", type=float, default=2.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=20.0)
    parser.add_argument("--boot-timeout-s", type=float, default=85.0)
    parser.add_argument("--discovery-timeout-s", type=float, default=120.0)
    parser.add_argument("--state-timeout-s", type=float, default=100.0)
    parser.add_argument("--route-timeout-s", type=float, default=140.0)
    parser.add_argument("--offline-timeout-s", type=float, default=45.0)
    parser.add_argument("--failover-timeout-s", type=float, default=140.0)
    parser.add_argument("--stable-polls", type=int, default=4)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--log-dir", required=True)
    return parser


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
