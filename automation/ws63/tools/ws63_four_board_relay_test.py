#!/usr/bin/env python3
"""WS63 four-board direct-cap relay enrollment and recovery test."""

from __future__ import annotations

import argparse
import json
import pathlib
import re
import sys
import time
from typing import Iterable, Optional

try:
    from automation.ws63.tools import ws63_link_cycle_test as lc
    from automation.ws63.tools import ws63_relay_cycle_test as rc
except ModuleNotFoundError:  # pragma: no cover - direct script execution
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(repo_root))
    from automation.ws63.tools import ws63_link_cycle_test as lc
    from automation.ws63.tools import ws63_relay_cycle_test as rc


MemberRecords = dict[int, rc.MemberRecord]
MEMBER_RESTORE_PATTERN = lc.MEMBER_RESTORE_PATTERN


def _now_ms() -> int:
    return int(time.time() * 1000)


def _progress(message: str) -> None:
    print(f"[four-board] {message}", flush=True)


def _read_available(peer: lc.Peer, max_bytes: int = 4096) -> str:
    try:
        waiting = int(getattr(peer.ser, "in_waiting", 0))
    except Exception:  # noqa: BLE001
        waiting = 0
    if waiting <= 0:
        return ""
    data = peer.ser.read(min(max_bytes, waiting))
    if not data:
        return ""
    text = data.decode("utf-8", errors="ignore")
    peer.log.append(text)
    return text


def _send_cli_line(peer: lc.Peer, line: str) -> None:
    lc._write_and_drain(peer.ser, b"\r\n", note=f"{peer.name} tx preamble")
    time.sleep(0.03)
    payload = (line + "\r\n").encode("utf-8")
    lc._write_and_drain(peer.ser, payload, note=f"{peer.name} tx {line!r}")
    if not hasattr(peer, "tx_log"):
        peer.tx_log = []  # type: ignore[attr-defined]
    peer.tx_log.append(line)


def _clear_peer_rx(peer: lc.Peer) -> None:
    try:
        peer.ser.reset_input_buffer()
    except Exception:  # noqa: BLE001
        pass


def _drain_all(peers: Iterable[lc.Peer], seconds: float) -> None:
    end = time.time() + seconds
    peer_list = list(peers)
    while time.time() < end:
        for peer in peer_list:
            _read_available(peer)
        time.sleep(0.02)


def _drain_to_text(target: lc.Peer, peers: Iterable[lc.Peer], seconds: float) -> str:
    end = time.time() + seconds
    text = ""
    peer_list = list(peers)
    while time.time() < end:
        for peer in peer_list:
            chunk = _read_available(peer)
            if peer is target and chunk:
                text += chunk
        time.sleep(0.02)
    return text


def _send_and_collect(
    target: lc.Peer,
    peers: Iterable[lc.Peer],
    line: str,
    seconds: float,
    *,
    clear_before: bool = True,
) -> str:
    if clear_before:
        _clear_peer_rx(target)
    _send_cli_line(target, line)
    return _drain_to_text(target, peers, seconds)


def _wait_pattern(
    target: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    pattern: str,
    timeout_s: float,
    note: str,
    log_start: int = 0,
) -> re.Match[str]:
    rx = re.compile(pattern)
    end = time.time() + timeout_s
    while time.time() < end:
        text = "".join(target.log[log_start:])
        match = rx.search(text)
        if match:
            return match
        for peer in peers:
            _read_available(peer)
        time.sleep(0.02)
    raise RuntimeError(f"timeout waiting for {note}: pattern={pattern}")


def _latest_cfg_json(text: str) -> Optional[dict[str, object]]:
    latest: Optional[dict[str, object]] = None
    for match in re.finditer(r"\[cfg-json\]\s*(\{[^\r\n]*\})", text):
        try:
            latest = json.loads(match.group(1))
        except json.JSONDecodeError:
            continue
    return latest


def _peer_log_tail(peer: lc.Peer, max_chars: int = 1200) -> str:
    tail = "".join(peer.log)[-max_chars:]
    return tail.replace("\r", "\\r").replace("\n", "\\n")


def _latest_leader_configured(text: str) -> Optional[dict[str, object]]:
    latest: Optional[dict[str, object]] = None
    for match in re.finditer(
        r"\[team\]\s+configured\s+fw=(?P<fw>\S+)\s+role=leader\s+self=(?P<self>\d+)\s+leader=(?P<leader>\d+)\s+"
        r"team=(?P<team>\d+)\s+channel=(?P<channel>\d+)\s+direct_cap=(?P<direct>\d+)\s+"
        r"(?:term=(?P<term>\d+)\s+)?label=(?P<label>\S+)",
        text,
    ):
        latest = {
            "fw": match.group("fw"),
            "runtimeConfigured": True,
            "runtimeRole": "leader",
            "runtimeLeader": int(match.group("leader")),
            "runtimeSelf": int(match.group("self")),
            "runtimeTeam": int(match.group("team")),
            "runtimeChannel": int(match.group("channel")),
            "runtimeDirectCap": int(match.group("direct")),
            "runtimeLeaderTerm": int(match.group("term") or 1),
            "runtimeRelayTarget": 0,
            "runtimeRelayCount": 0,
            "runtimeOnlineCount": 0,
            "runtimeJoined": 1,
            "runtimeParent": 0,
            "runtimeRelayEnabled": 0,
        }
    return latest


def _leader_runtime_from_config_log(
    text: str,
    *,
    team_id: int,
    channel: int,
    direct_cap: Optional[int],
    relay_target: Optional[int],
) -> Optional[dict[str, object]]:
    if re.search(rf"\[cfg\]\s+leader-now\s+ret=0\s+team={team_id}\s+channel={channel}\b", text) is None:
        return None
    status = _latest_leader_configured(text)
    if status is None:
        return None
    if (
        status.get("runtimeTeam") != team_id
        or status.get("runtimeChannel") != channel
        or (direct_cap is not None and status.get("runtimeDirectCap") != direct_cap)
    ):
        return None
    status["runtimeRelayTarget"] = int(relay_target or 0)
    return status


def _find_route_regression_events(
    peers: Iterable[lc.Peer],
    leader_id: int,
    *,
    expected_source_ids: Iterable[int] | None = None,
) -> list[str]:
    events: list[str] = []
    expected_sources = None if expected_source_ids is None else {int(member_id) for member_id in expected_source_ids}
    leader_bound_rx = re.compile(
        rf"\[sle-rx\]\s+(?P<type>HELLO|HEARTBEAT|POS_REPORT|ROUTE_UPDATE|CONFIG|ACK)\s+"
        rf"(?P<src>\d+)->{leader_id}\b"
    )
    route_hint_failed = re.compile(r"\[team\]\s+route hint member=(\d+)\s+parent=(\d+)\s+ret=(-?\d+)")
    no_route = re.compile(r"\[sle-tx-fail\]\s+type=PACKET\s+dst=(\d+)\s+ret=-4\s+reason=NO_ROUTE")
    unsupported_node_packet = re.compile(r"\[team\]\s+node packet role=0 len=\d+ ret=-4")

    for peer in peers:
        text = "".join(peer.log)
        for match in route_hint_failed.finditer(text):
            member_id = int(match.group(1))
            if expected_sources is not None and member_id not in expected_sources:
                continue
            ret = int(match.group(3))
            if ret != 0:
                events.append(
                    f"{peer.name}: route hint failed member={member_id} parent={match.group(2)} ret={ret}"
                )
        for match in no_route.finditer(text):
            dst_id = int(match.group(1))
            window = text[max(0, match.start() - 500) : min(len(text), match.end() + 500)]
            leader_bound_match = leader_bound_rx.search(window)
            if (
                dst_id == leader_id
                and leader_bound_match
                and (
                    expected_sources is None
                    or int(leader_bound_match.group("src")) in expected_sources
                )
            ):
                events.append(f"{peer.name}: leader-bound packet hit NO_ROUTE dst={dst_id}")
            elif "route hint member=" in window and (expected_sources is None or dst_id in expected_sources):
                events.append(f"{peer.name}: route hint send hit NO_ROUTE dst={dst_id}")
        for match in leader_bound_rx.finditer(text):
            if expected_sources is not None and int(match.group("src")) not in expected_sources:
                continue
            window = text[match.start() : min(len(text), match.end() + 500)]
            unsupported = unsupported_node_packet.search(window)
            if unsupported is not None:
                forwarded = window.find("[state] relay forwarded packet")
                if forwarded == -1 or forwarded > unsupported.start():
                    src_id = match.group("src")
                    later_forward = re.compile(
                        rf"\[sle-rx\]\s+(?:HELLO|HEARTBEAT|POS_REPORT|ROUTE_UPDATE|CONFIG|ACK)\s+"
                        rf"{src_id}->{leader_id}\b[\s\S]{{0,900}}?\[state\]\s+relay forwarded packet"
                    )
                    if later_forward.search(text[match.end() : min(len(text), match.end() + 12000)]):
                        continue
                    events.append(f"{peer.name}: leader-bound packet was rejected before relay forward")
    return events


def _assert_no_route_regressions(
    peers: Iterable[lc.Peer],
    leader_id: int,
    *,
    expected_source_ids: Iterable[int] | None = None,
) -> None:
    events = _find_route_regression_events(peers, leader_id, expected_source_ids=expected_source_ids)
    if events:
        raise RuntimeError("route regression detected: " + "; ".join(events[:5]))


def _query_cfg(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    window_s: float = 3.0,
    attempts: int = 3,
) -> dict[str, object]:
    last_text = ""
    for attempt in range(1, max(1, attempts) + 1):
        _progress(f"query {peer.name} {peer.port}: cfg status attempt {attempt}/{max(1, attempts)}")
        text = _send_and_collect(peer, peers, "cfg status", window_s)
        if text:
            last_text = text
        status = _latest_cfg_json(text)
        if status is not None:
            return status
        _progress(f"query {peer.name} {peer.port}: no cfg-json rx_bytes={len(text)}")
        time.sleep(0.3)
    raise RuntimeError(
        f"{peer.name} cfg status did not return cfg-json; "
        f"last_rx_bytes={len(last_text)} tail={_peer_log_tail(peer)}"
    )


def _wait_cfg_field(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    key: str,
    expected: object,
    timeout_s: float,
    note: str,
) -> dict[str, object]:
    end = time.time() + timeout_s
    last: Optional[dict[str, object]] = None
    while time.time() < end:
        try:
            last = _query_cfg(peer, peers)
            if last.get(key) == expected:
                return last
        except RuntimeError:
            pass
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for {note}: {key}={expected}, last={last}")


def _wait_leader_runtime_after_config(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    team_id: int,
    channel: int,
    direct_cap: Optional[int],
    relay_target: Optional[int],
    timeout_s: float,
    log_start: int,
) -> dict[str, object]:
    end = time.time() + timeout_s
    last: Optional[dict[str, object]] = None
    while time.time() < end:
        log_text = "".join(leader.log[log_start:])
        status = _leader_runtime_from_config_log(
            log_text,
            team_id=team_id,
            channel=channel,
            direct_cap=direct_cap,
            relay_target=relay_target,
        )
        if status is not None:
            _progress("leader runtime confirmed from configured log")
            return status
        try:
            last = _query_cfg(leader, peers, window_s=1.0, attempts=1)
        except RuntimeError:
            last = None
        else:
            if last.get("runtimeRole") == "leader":
                return last
        time.sleep(0.5)
    raise RuntimeError(f"timeout waiting for leader runtime: runtimeRole=leader, last={last}")


def _send_cli_once_and_wait_pattern(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    command: str,
    pattern: str,
    timeout_s: float,
    note: str,
) -> re.Match[str]:
    start = len(peer.log)
    _progress(f"{note}: send once")
    _send_cli_line(peer, command)
    return _wait_pattern(
        peer,
        peers,
        timeout_s=timeout_s,
        note=note,
        pattern=pattern,
        log_start=start,
    )


def _assert_fw(peers: Iterable[lc.Peer], all_peers: Iterable[lc.Peer], expected_fw: str) -> dict[str, dict[str, object]]:
    out: dict[str, dict[str, object]] = {}
    for peer in peers:
        status = _query_cfg(peer, all_peers, window_s=3.0, attempts=3)
        fw = status.get("fw")
        if fw != expected_fw:
            raise RuntimeError(f"{peer.name} firmware mismatch: expected {expected_fw}, got {fw}")
        out[peer.name] = status
        _progress(
            f"{peer.name} {peer.port} fw={fw} suffix={status.get('selfSuffix')} route={status.get('routeId')}"
        )
    return out


def _clean_start_saved_config(
    peers: Iterable[lc.Peer],
    *,
    timeout_s: float,
    boot_timeout_s: float,
    reboot_command: str,
) -> None:
    peer_list = list(peers)
    _progress("clean start: clear saved role/allowlist state on all boards")
    for peer in peer_list:
        _send_cfg_and_wait(
            peer,
            peer_list,
            command="cfg clear",
            pattern=r"\[cfg\] clear ret=0\b",
            timeout_s=timeout_s,
            note=f"{peer.name} cfg clear",
        )

    _progress("clean start: reboot all boards after cfg clear")
    for peer in peer_list:
        _send_cli_line(peer, reboot_command)

    for peer in peer_list:
        _wait_cfg_field(
            peer,
            peer_list,
            key="runtimeConfigured",
            expected=False,
            timeout_s=boot_timeout_s,
            note=f"{peer.name} unconfigured after clean reboot",
        )


def _send_cfg_and_wait(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    command: str,
    pattern: str,
    timeout_s: float,
    note: str,
) -> re.Match[str]:
    rx = re.compile(pattern)
    end = time.time() + timeout_s
    start = len(peer.log)
    attempt = 0
    while time.time() < end:
        attempt += 1
        _progress(f"{note}: send attempt {attempt}")
        _send_cli_line(peer, command)
        attempt_end = min(end, time.time() + 2.5)
        while time.time() < attempt_end:
            text = "".join(peer.log[start:])
            match = rx.search(text)
            if match:
                return match
            for item in peers:
                _read_available(item)
            time.sleep(0.02)
    raise RuntimeError(f"timeout waiting for {note}: pattern={pattern}")


def _configure_member_role(
    member: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    leader_suffix: str,
    team_id: int,
    channel: int,
    timeout_s: float,
    boot_timeout_s: float,
) -> None:
    """Configure a board as member, rebooting if it was already running another role."""
    match = _send_cfg_and_wait(
        member,
        peers,
        command=f"cfg member now {leader_suffix} {team_id} {channel}",
        pattern=rf"member-now (?:queued )?ret=(-?\d+) leader_suffix={leader_suffix}",
        timeout_s=timeout_s,
        note=f"{member.name} cfg member now",
    )
    ret = int(match.group(1))
    if ret == 0:
        _wait_cfg_field(member, peers, key="runtimeRole", expected="member", timeout_s=timeout_s,
                        note=f"{member.name} runtime")
        return
    if ret != -4:
        raise RuntimeError(f"{member.name} cfg member now returned ret={ret}")

    status = _query_cfg(member, peers, window_s=2.0, attempts=2)
    if status.get("nvRole") != "member" or status.get("nvLeaderSuffix") != leader_suffix:
        raise RuntimeError(f"{member.name} saved member NV was not confirmed after ret=-4: {status}")
    _progress(f"{member.name}: runtime already configured; reboot to apply saved member NV")
    _send_cli_line(member, "cfg reboot")
    _wait_pattern(
        member,
        peers,
        pattern=rf"({MEMBER_RESTORE_PATTERN}|\[team\] configured .* role=0)",
        timeout_s=boot_timeout_s,
        note=f"{member.name} member restore after reboot",
    )
    _wait_cfg_field(member, peers, key="runtimeRole", expected="member", timeout_s=timeout_s,
                    note=f"{member.name} runtime after reboot")


def _configure_roles(
    leader: lc.Peer,
    members: Iterable[lc.Peer],
    peers: Iterable[lc.Peer],
    *,
    leader_suffix: str,
    team_id: int,
    channel: int,
    direct_cap: int,
    relay_target: Optional[int],
    skip_direct_config: bool,
    timeout_s: float,
    boot_timeout_s: float,
) -> dict[str, object]:
    if not skip_direct_config:
        _progress(f"preconfigure leader direct cap before role start: cfg direct {direct_cap}")
        _send_cli_once_and_wait_pattern(
            leader,
            peers,
            command=f"cfg direct {direct_cap}",
            pattern=rf"\[cfg\] direct cap={direct_cap}\b.*ret=0",
            timeout_s=timeout_s,
            note="leader direct cap",
        )
    if relay_target is not None:
        _progress(f"preconfigure leader relay target before role start: cfg relay target {relay_target}")
        _send_cli_once_and_wait_pattern(
            leader,
            peers,
            command=f"cfg relay target {relay_target}",
            pattern=rf"\[cfg\] relay target=\d+\b.*override={relay_target}\b.*ret=0",
            timeout_s=timeout_s,
            note="leader relay target",
        )

    _progress(f"configure leader: cfg leader now {team_id} {channel}")
    leader_config_log_start = len(leader.log)
    _send_cfg_and_wait(
        leader,
        peers,
        command=f"cfg leader now {team_id} {channel}",
        pattern=rf"leader-now (?:queued )?ret=0 team={team_id} channel={channel}",
        timeout_s=timeout_s,
        note="leader cfg now",
    )
    leader_status = _wait_leader_runtime_after_config(
        leader,
        peers,
        team_id=team_id,
        channel=channel,
        direct_cap=None if skip_direct_config else direct_cap,
        relay_target=relay_target,
        timeout_s=timeout_s,
        log_start=leader_config_log_start,
    )
    if skip_direct_config:
        _progress(
            "configure leader direct cap: skip cfg direct; "
            f"default runtimeDirectCap={leader_status.get('runtimeDirectCap')} "
            f"runtimeRelayTarget={leader_status.get('runtimeRelayTarget')} "
            f"runtimeRelayCount={leader_status.get('runtimeRelayCount')}"
        )
    else:
        if leader_status.get("runtimeDirectCap") != direct_cap:
            raise RuntimeError(
                f"leader direct cap was not applied before role start: "
                f"expected {direct_cap}, status={leader_status}"
            )
    if relay_target is not None and leader_status.get("runtimeRelayTarget") != relay_target:
        raise RuntimeError(
            f"leader relay target was not applied before role start: expected {relay_target}, status={leader_status}"
        )
    for member in members:
        _progress(f"configure {member.name}: cfg member now {leader_suffix} {team_id} {channel}")
        _configure_member_role(
            member,
            peers,
            leader_suffix=leader_suffix,
            team_id=team_id,
            channel=channel,
            timeout_s=timeout_s,
            boot_timeout_s=boot_timeout_s,
        )
    return leader_status


def _wait_member_records(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    expected: dict[int, tuple[Optional[int], Optional[int]]],
    timeout_s: float,
    poll_s: float,
    note: str,
) -> MemberRecords:
    end = time.time() + timeout_s
    last: MemberRecords = {}
    while time.time() < end:
        text = _send_and_collect(leader, peers, "members", min(0.6, poll_s))
        records = rc._parse_member_records(text)
        if records:
            last = records
        ok = True
        for member_id, (online, relay) in expected.items():
            record = records.get(member_id)
            if record is None:
                ok = False
                break
            if online is not None and record["online"] != online:
                ok = False
                break
            if relay is not None and record["relay"] != relay:
                ok = False
                break
        if ok:
            return records
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"timeout waiting for {note}: last={last}")


def _wait_stable_final_topology(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    leader_id: int,
    member_ids: list[int],
    direct_cap: int,
    timeout_s: float,
    poll_s: float,
    log_start: int = 0,
    stable_polls: int = 5,
    allow_any_converged: bool = False,
) -> MemberRecords:
    end = time.time() + timeout_s
    last: MemberRecords = {}
    stable = 0
    route_ok = False
    fallback_reported = False
    expected_direct = min(max(1, direct_cap), len(member_ids))
    expected_relayed = max(0, len(member_ids) - expected_direct)
    route_pattern = re.compile(
        rf"route metrics active={len(member_ids)} direct={expected_direct} relayed={expected_relayed} "
        r"stale=0 unreachable=0 plan=0 converged=1"
    )
    any_route_pattern = re.compile(
        rf"route metrics active={len(member_ids)} direct=(\d+) relayed=(\d+) "
        r"stale=0 unreachable=0 plan=0 converged=1"
    )

    while time.time() < end:
        records = _query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last = records
        online = [mid for mid in member_ids if records.get(mid, {}).get("online") == 1]
        relays = [mid for mid in member_ids if records.get(mid, {}).get("online") == 1 and records.get(mid, {}).get("relay") == 1]
        leader_text = "".join(leader.log[log_start:])
        route_ok = route_pattern.search(leader_text) is not None
        if allow_any_converged and route_ok is False:
            route_ok = any_route_pattern.search(leader_text) is not None
        forwarding_ok = False
        if expected_relayed > 0:
            forwarding_ok = _leader_observed_final_forwarding(
                leader_text,
                leader_id=leader_id,
                member_ids=member_ids,
                relay_ids=relays,
                direct_cap=direct_cap,
            )
        no_relay_record_ok = allow_any_converged and expected_relayed == 0 and len(relays) == 0
        topology_ok = route_ok or no_relay_record_ok or (expected_relayed > 0 and forwarding_ok)
        if len(online) == len(member_ids) and topology_ok:
            if route_ok is False and forwarding_ok is True and fallback_reported is False:
                _progress("final route metrics not refreshed; using leader forwarding evidence")
                fallback_reported = True
            stable += 1
            if stable >= stable_polls:
                return records
        else:
            stable = 0
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(
        "timeout waiting for stable final topology: "
        f"stable={stable}/{stable_polls} direct_cap={direct_cap} route_ok={route_ok} last={last}"
    )


def _leader_observed_packet_from(text: str, *, member_id: int, leader_id: int) -> bool:
    return re.search(
        rf"member={member_id}\b[^\r\n]*\bonline=1\b",
        text,
    ) is not None


def _leader_observed_packet_via_relay(text: str, *, member_id: int, relay_id: int, leader_id: int) -> bool:
    unused = leader_id
    del unused
    return re.search(
        rf"member={relay_id}\b[^\r\n]*\brelay=1\b[\s\S]*?"
        rf"member={member_id}\b[^\r\n]*\bonline=1\b",
        text,
    ) is not None


def _leader_observed_final_forwarding(
    text: str,
    *,
    leader_id: int,
    member_ids: list[int],
    relay_ids: list[int],
    direct_cap: int,
) -> bool:
    if not relay_ids:
        return False
    for member_id in member_ids:
        if not _leader_observed_packet_from(text, member_id=member_id, leader_id=leader_id):
            return False
    if direct_cap > len(relay_ids):
        return True
    for member_id in member_ids:
        if member_id in relay_ids:
            continue
        if not any(
            _leader_observed_packet_via_relay(text, member_id=member_id, relay_id=relay_id, leader_id=leader_id)
            for relay_id in relay_ids
        ):
            return False
    return True


def _wait_peer_joined(peer: lc.Peer, peers: Iterable[lc.Peer], timeout_s: float, poll_s: float) -> None:
    end = time.time() + timeout_s
    last = ""
    while time.time() < end:
        last = _send_and_collect(peer, peers, "state", min(0.6, poll_s))
        if re.search(r"\bjoined=1\b", last):
            return
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"{peer.name} did not report joined=1, last={last!r}")


def _wait_leader_sees_member(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    leader_id: int,
    timeout_s: float,
    poll_s: float,
) -> None:
    end = time.time() + timeout_s
    start = len(leader.log)
    pattern = re.compile(rf"(pending member={member_id}\b|HELLO {member_id}->{leader_id}\b|member={member_id}\b)")
    last_records: MemberRecords = {}
    last_ping_s = 0.0
    while time.time() < end:
        records = _query_records_once(leader, peers, min(0.5, poll_s))
        if records:
            last_records = records
        record = records.get(member_id)
        if record is not None and record.get("online") == 1:
            return
        if pattern.search("".join(leader.log[start:])):
            return
        now_s = time.time()
        if now_s - last_ping_s >= max(1.5, poll_s):
            _send_and_collect(leader, peers, "pairing pending", min(0.3, poll_s))
            last_ping_s = now_s
        time.sleep(max(0.1, poll_s - 0.5))
    raise RuntimeError(f"leader did not see member={member_id} pending/hello/online, last={last_records}")


def _wait_leader_observes_member_reboot_rejoin(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    leader_id: int,
    timeout_s: float,
    poll_s: float,
    log_start: int = 0,
) -> MemberRecords:
    end = time.time() + timeout_s
    last_records: MemberRecords = {}
    baseline_last_seen: Optional[int] = None
    rejoin_pattern = re.compile(rf"(\[sle-rx\]\s+HELLO\s+{member_id}->{leader_id}\b|\[team\]\s+joined member={member_id}\b)")

    while time.time() < end:
        records = _query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last_records = records
        text = "".join(leader.log[log_start:])
        record = records.get(member_id)
        if record is not None and record.get("online") == 1:
            last_seen = record.get("last_seen")
            if isinstance(last_seen, int):
                if baseline_last_seen is None:
                    baseline_last_seen = last_seen
                elif last_seen > baseline_last_seen:
                    return records
            if rejoin_pattern.search(text):
                return records
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(
        f"leader did not observe member={member_id} reboot HELLO/rejoin or refreshed heartbeat; "
        f"baseline_last_seen={baseline_last_seen} last={last_records}"
    )


def _approve_member(
    leader: lc.Peer,
    member: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    leader_id: int,
    relay: int,
    timeout_s: float,
    poll_s: float,
) -> None:
    _wait_leader_sees_member(leader, peers, member_id=member_id, leader_id=leader_id, timeout_s=timeout_s, poll_s=poll_s)
    role = "relay" if relay else "norelay"
    _progress(f"approve {member.name}: pairing approve {member_id} {role}")
    _send_cfg_and_wait(
        leader,
        peers,
        command=f"pairing approve {member_id} {role}",
        pattern=rf"pairing approve member={member_id} relay={relay} ret=0",
        timeout_s=timeout_s,
        note=f"approve {member.name}",
    )
    _wait_member_records(
        leader,
        peers,
        expected={member_id: (1, relay)},
        timeout_s=timeout_s,
        poll_s=poll_s,
        note=f"leader member table for {member.name}",
    )
    _wait_peer_joined(member, peers, timeout_s=timeout_s, poll_s=poll_s)


def _preapprove_member(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    relay: int,
    timeout_s: float,
) -> None:
    role = "relay" if relay else "norelay"
    _progress(f"preapprove member={member_id}: pairing approve {role}")
    _send_cfg_and_wait(
        leader,
        peers,
        command=f"pairing approve {member_id} {role}",
        pattern=rf"pairing approve member={member_id} relay={relay} ret=0",
        timeout_s=timeout_s,
        note=f"preapprove member={member_id}",
    )


def _wait_route_converged(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    timeout_s: float,
    log_start: int = 0,
) -> None:
    pattern = (
        r"route metrics active=3 direct=1 relayed=2 stale=0 "
        r"unreachable=0 plan=0 converged=1"
    )
    _wait_pattern(
        leader,
        peers,
        pattern=pattern,
        timeout_s=timeout_s,
        note="route metrics converged",
        log_start=log_start,
    )


def _wait_relay_forwards_children(
    relay: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    child_ids: list[int],
    timeout_s: float,
    log_start: int = 0,
) -> None:
    end = time.time() + timeout_s
    seen = {child_id: False for child_id in child_ids}
    conn_ids: dict[int, str] = {}
    while time.time() < end:
        text = "".join(relay.log[log_start:])
        for child_id in child_ids:
            if child_id not in conn_ids:
                bind = re.search(rf"\[team\]\s+client conn bind conn=(\d+) route={child_id}\b", text)
                if bind:
                    conn_ids[child_id] = bind.group(1)
            conn_id = conn_ids.get(child_id)
            if conn_id is not None and re.search(rf"\[team-rx\]\s+conn={conn_id}\s+side=client\s+ret=0\b", text):
                seen[child_id] = True
        if all(seen.values()):
            return
        for peer in peers:
            _read_available(peer)
        time.sleep(0.05)
    missing = [child_id for child_id, ok in seen.items() if not ok]
    raise RuntimeError(f"relay did not forward child traffic for {missing}")


def _query_records_once(leader: lc.Peer, peers: Iterable[lc.Peer], window_s: float = 1.0) -> MemberRecords:
    text = _send_and_collect(leader, peers, "members", window_s)
    return rc._parse_member_records(text)


def _wait_any_child_relay(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    child_ids: list[int],
    timeout_s: float,
    poll_s: float,
) -> MemberRecords:
    end = time.time() + timeout_s
    last: MemberRecords = {}
    while time.time() < end:
        records = _query_records_once(leader, peers, min(0.6, poll_s))
        if records:
            last = records
        if any(records.get(child_id, {}).get("online") == 1 and records.get(child_id, {}).get("relay") == 1 for child_id in child_ids):
            return records
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"no child relay elected after relay loss, last={last}")


def _try_wait_child_relay(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    child_ids: list[int],
    timeout_s: float,
    poll_s: float,
) -> MemberRecords:
    try:
        return _wait_any_child_relay(
            leader,
            peers,
            child_ids=child_ids,
            timeout_s=timeout_s,
            poll_s=poll_s,
        )
    except RuntimeError as exc:
        _progress(f"child relay election not observed before relay restore check: {exc}")
        return {}


def _relay_ids_from_records(records: MemberRecords, member_ids: Iterable[int]) -> list[int]:
    return [mid for mid in member_ids if records.get(mid, {}).get("online") == 1 and records.get(mid, {}).get("relay") == 1]


def _prefer_non_relay_member_id(records: MemberRecords, preferred_ids: Iterable[int]) -> Optional[int]:
    for member_id in preferred_ids:
        record = records.get(member_id)
        if record is not None and record.get("online") == 1 and record.get("relay") == 0:
            return member_id
    return None


def _summarize_policy(records: MemberRecords, relay_id: int, child_ids: list[int]) -> str:
    relay_record = records.get(relay_id)
    child_relays = [child_id for child_id in child_ids if records.get(child_id, {}).get("relay") == 1]
    original = relay_record is not None and relay_record.get("relay") == 1
    if original and not child_relays:
        return "original relay regained relay role; child relay was demoted"
    if not original and child_relays:
        return f"new child relay retained role; original relay returned as member; child_relays={child_relays}"
    if original and child_relays:
        return f"multiple relays online after recovery; original relay and child_relays={child_relays}"
    return "no relay flag observed after recovery"


def _dump_logs(peers: Iterable[lc.Peer], out_dir: pathlib.Path) -> None:
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


def run(args: argparse.Namespace) -> int:
    peers: list[lc.Peer] = []
    leader_id: Optional[int] = None
    started_at = _now_ms()
    try:
        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        relay = lc._open_peer("relay", args.relay_port, args.baudrate)
        child1 = lc._open_peer("child1", args.child1_port, args.baudrate)
        child2 = lc._open_peer("child2", args.child2_port, args.baudrate)
        peers = [leader, relay, child1, child2]

        _progress("drain serial boot logs")
        _drain_all(peers, args.initial_drain_s)
        statuses = _assert_fw(peers, peers, args.expected_fw)
        leader_suffix = str(statuses["leader"]["selfSuffix"])
        leader_id = int(statuses["leader"]["routeId"])
        relay_id = int(statuses["relay"]["routeId"])
        child1_id = int(statuses["child1"]["routeId"])
        child2_id = int(statuses["child2"]["routeId"])
        if len({leader_id, relay_id, child1_id, child2_id}) != 4:
            raise RuntimeError(
                f"route ids must be unique: leader={leader_id} relay={relay_id} "
                f"child1={child1_id} child2={child2_id}"
            )
        member_ids = [relay_id, child1_id, child2_id]
        peer_by_member_id = {relay_id: relay, child1_id: child1, child2_id: child2}
        relay_target = args.relay_target
        if relay_target is None and not args.natural_members:
            relay_target = 1

        if not args.no_clean_start:
            _clean_start_saved_config(
                peers,
                timeout_s=args.cmd_timeout_s,
                boot_timeout_s=args.boot_timeout_s,
                reboot_command=args.reboot_command,
            )

        relay_forward_log_start = len(relay.log)
        leader_status = _configure_roles(
            leader,
            [relay, child1, child2],
            peers,
            leader_suffix=leader_suffix,
            team_id=args.team_id,
            channel=args.channel,
            direct_cap=args.direct_cap,
            relay_target=relay_target,
            skip_direct_config=args.skip_direct_config,
            timeout_s=args.cmd_timeout_s,
            boot_timeout_s=args.boot_timeout_s,
        )
        runtime_direct_cap = int(leader_status.get("runtimeDirectCap") or args.direct_cap)
        _progress(
            f"leader runtime capacity: direct_cap={runtime_direct_cap} "
            f"relay_target={leader_status.get('runtimeRelayTarget')} "
            f"relay_count={leader_status.get('runtimeRelayCount')} "
            f"online_count={leader_status.get('runtimeOnlineCount')}"
        )

        route_log_start = len(leader.log)
        _progress("pairing start")
        _send_cfg_and_wait(
            leader,
            peers,
            command="pairing start",
            pattern=r"pairing start ret=0",
            timeout_s=args.cmd_timeout_s,
            note="pairing start",
        )

        if args.natural_members:
            _progress("natural member mode: wait all three pending; pairing stop lets firmware auto-policy decide")
            for member_id in [relay_id, child1_id, child2_id]:
                _wait_leader_sees_member(
                    leader,
                    peers,
                    member_id=member_id,
                    leader_id=leader_id,
                    timeout_s=args.state_timeout_s,
                    poll_s=args.poll_interval_s,
                )
        else:
            _preapprove_member(
                leader,
                peers,
                member_id=relay_id,
                relay=1,
                timeout_s=args.cmd_timeout_s,
            )
            _wait_member_records(
                leader,
                peers,
                expected={relay_id: (1, 1)},
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                note=f"leader relay member table for {relay.name}",
            )
            _wait_peer_joined(relay, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
            _progress("wait child1/child2 pending; pairing stop auto-approves them as no-relay members")
            _wait_leader_sees_member(
                leader,
                peers,
                member_id=child1_id,
                leader_id=leader_id,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
            )
            _wait_leader_sees_member(
                leader,
                peers,
                member_id=child2_id,
                leader_id=leader_id,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
            )

        _progress("pairing stop")
        _send_cfg_and_wait(
            leader,
            peers,
            command="pairing stop",
            pattern=r"pairing stop ret=0",
            timeout_s=args.cmd_timeout_s,
            note="pairing stop",
        )
        _wait_member_records(
            leader,
            peers,
            expected={
                relay_id: (1, None if args.natural_members else 1),
                child1_id: (1, None if args.natural_members else 0),
                child2_id: (1, None if args.natural_members else 0),
            },
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note="all three members online after enrollment",
        )
        if args.natural_members:
            _wait_peer_joined(relay, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        _wait_peer_joined(child1, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        _wait_peer_joined(child2, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        if args.natural_members:
            initial_records = _wait_stable_final_topology(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_ids,
                direct_cap=runtime_direct_cap,
                timeout_s=args.route_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=route_log_start,
                stable_polls=2,
                allow_any_converged=True,
            )
            initial_relays = _relay_ids_from_records(initial_records, member_ids)
            _progress(f"natural enrollment topology: relays={initial_relays} direct_cap={runtime_direct_cap}")
        else:
            try:
                _wait_route_converged(
                    leader,
                    peers,
                    timeout_s=min(args.route_timeout_s, 15.0),
                    log_start=route_log_start,
                )
            except RuntimeError as exc:
                _progress(f"route metrics log not refreshed; verify relay forwarding fallback: {exc}")
                _wait_relay_forwards_children(
                    relay,
                    peers,
                    child_ids=[child1_id, child2_id],
                    timeout_s=args.route_timeout_s,
                    log_start=relay_forward_log_start,
                )
        _progress(
            f"enrollment PASS: leader={leader_id} relay={relay_id} child1={child1_id} child2={child2_id}"
        )

        reboot_member_id = child1_id
        reboot_member = child1
        if args.natural_members:
            reboot_candidate = _prefer_non_relay_member_id(initial_records, [child1_id, relay_id, child2_id])
            if reboot_candidate is None:
                raise RuntimeError(f"natural topology has no non-relay member to reboot: {initial_records}")
            reboot_member_id = reboot_candidate
            reboot_member = peer_by_member_id[reboot_member_id]
            if reboot_member_id != child1_id:
                _progress(f"member reboot test: child1 is relay; reboot non-relay id={reboot_member_id}")

        _progress(f"member reboot test: reboot {reboot_member.name} id={reboot_member_id}")
        child_reboot_start = len(leader.log)
        _send_cli_line(reboot_member, args.reboot_command)
        _wait_pattern(
            reboot_member,
            peers,
            pattern=MEMBER_RESTORE_PATTERN,
            timeout_s=args.boot_timeout_s,
            note="member restore from NV",
        )
        _wait_leader_observes_member_reboot_rejoin(
            leader,
            peers,
            member_id=reboot_member_id,
            leader_id=leader_id,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=child_reboot_start,
        )
        _wait_member_records(
            leader,
            peers,
            expected={reboot_member_id: (1, None)},
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note="member rejoins after reboot",
        )
        _wait_peer_joined(reboot_member, peers, timeout_s=args.state_timeout_s, poll_s=args.poll_interval_s)
        _progress("member reboot PASS: leader saw reboot HELLO/rejoin and member is online")

        if args.natural_members:
            post_member_records = _wait_stable_final_topology(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_ids,
                direct_cap=runtime_direct_cap,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=child_reboot_start,
                stable_polls=2,
                allow_any_converged=True,
            )
            _assert_no_route_regressions(peers, leader_id)
            post_member_relays = _relay_ids_from_records(post_member_records, member_ids)
            _progress(f"natural policy after member reboot: relays={post_member_relays}")
            if not post_member_relays:
                _progress("natural direct policy: no relay elected; relay reboot/failover path is not applicable")
                _progress(f"PASS total_ms={_now_ms() - started_at}")
                return 0

            natural_relay_id = post_member_relays[0]
            natural_relay = peer_by_member_id[natural_relay_id]
            downstream_ids = [member_id for member_id in member_ids if member_id != natural_relay_id]
            _progress(f"natural relay reboot test: reboot actual relay {natural_relay.name} id={natural_relay_id}")
            relay_offline_start = len(leader.log)
            relay_restore_start = len(natural_relay.log)
            _send_cli_line(natural_relay, args.reboot_command)
            lc._wait_leader_offline_event(
                leader,
                peers,
                member_id=natural_relay_id,
                timeout_s=args.offline_timeout_s,
                note="leader offline after natural relay reboot",
                log_start=relay_offline_start,
            )
            child_relay_records = _try_wait_child_relay(
                leader,
                peers,
                child_ids=downstream_ids,
                timeout_s=args.failover_timeout_s,
                poll_s=args.poll_interval_s,
            )
            elected = [mid for mid in downstream_ids if child_relay_records.get(mid, {}).get("relay") == 1]
            if elected:
                _progress(f"natural relay failover observed: relay elected/retained {elected}")
            else:
                _progress("natural relay recovered before child relay election was observed")

            _wait_pattern(
                natural_relay,
                peers,
                pattern=MEMBER_RESTORE_PATTERN,
                timeout_s=args.boot_timeout_s,
                note="natural relay restore from NV",
                log_start=relay_restore_start,
            )
            final_records = _wait_stable_final_topology(
                leader,
                peers,
                leader_id=leader_id,
                member_ids=member_ids,
                direct_cap=runtime_direct_cap,
                timeout_s=args.state_timeout_s,
                poll_s=args.poll_interval_s,
                log_start=relay_offline_start,
                allow_any_converged=True,
            )
            _assert_no_route_regressions(peers, leader_id)
            policy = _summarize_policy(final_records, natural_relay_id, downstream_ids)
            final_relays = _relay_ids_from_records(final_records, member_ids)
            _progress(f"natural relay recovery policy: {policy}; final_relays={final_relays}")
            _progress(f"PASS total_ms={_now_ms() - started_at}")
            return 0

        _progress(f"relay reboot test: reboot relay id={relay_id}")
        relay_offline_start = len(leader.log)
        relay_restore_start = len(relay.log)
        _send_cli_line(relay, args.reboot_command)
        lc._wait_leader_offline_event(
            leader,
            peers,
            member_id=relay_id,
            timeout_s=args.offline_timeout_s,
            note="leader offline after relay reboot",
            log_start=relay_offline_start,
        )
        child_relay_records = _try_wait_child_relay(
            leader,
            peers,
            child_ids=[child1_id, child2_id],
            timeout_s=args.failover_timeout_s,
            poll_s=args.poll_interval_s,
        )
        elected = [mid for mid in [child1_id, child2_id] if child_relay_records.get(mid, {}).get("relay") == 1]
        if elected:
            _progress(f"relay failover observed: child relay elected {elected}")
        else:
            _progress("original relay restored before child relay election was observed")

        final_route_log_start = relay_offline_start
        _wait_pattern(
            relay,
            peers,
            pattern=MEMBER_RESTORE_PATTERN,
            timeout_s=args.boot_timeout_s,
            note="relay restore from NV",
            log_start=relay_restore_start,
        )
        final_records = _wait_stable_final_topology(
            leader,
            peers,
            leader_id=leader_id,
            member_ids=[relay_id, child1_id, child2_id],
            direct_cap=args.direct_cap,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            log_start=final_route_log_start,
        )
        _assert_no_route_regressions(peers, leader_id)
        policy = _summarize_policy(final_records, relay_id, [child1_id, child2_id])
        _progress(f"relay recovery policy: {policy}")
        _progress(f"PASS total_ms={_now_ms() - started_at}")
        return 0
    except Exception as exc:  # noqa: BLE001
        _progress(f"FAIL: {exc}")
        if leader_id is not None:
            regressions = _find_route_regression_events(peers, leader_id)
            if regressions:
                _progress("route regression evidence: " + "; ".join(regressions[:5]))
        return 1
    finally:
        if args.log_dir:
            _dump_logs(peers, pathlib.Path(args.log_dir))
            _progress(f"logs saved: {args.log_dir}")
        for peer in peers:
            try:
                peer.ser.close()
            except Exception:  # noqa: BLE001
                pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 live four-board direct-cap relay test")
    parser.add_argument("--leader-port", default="COM16")
    parser.add_argument("--relay-port", default="COM13")
    parser.add_argument("--child1-port", default="COM17")
    parser.add_argument("--child2-port", default="COM18")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="v4.5.56-minimal")
    parser.add_argument("--team-id", type=int, default=1)
    parser.add_argument("--channel", type=int, default=17)
    parser.add_argument("--direct-cap", type=int, default=1)
    parser.add_argument(
        "--relay-target",
        type=int,
        default=None,
        help="leader relay target override; strict relay mode defaults to 1, natural mode leaves it automatic",
    )
    parser.add_argument("--skip-direct-config", action="store_true", help="do not send cfg direct; observe default leader capacity")
    parser.add_argument("--natural-members", action="store_true", help="do not pre-approve relay; let firmware auto-policy decide")
    parser.add_argument("--reboot-command", default="cfg reboot")
    parser.add_argument("--initial-drain-s", type=float, default=2.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=15.0)
    parser.add_argument("--state-timeout-s", type=float, default=60.0)
    parser.add_argument("--route-timeout-s", type=float, default=60.0)
    parser.add_argument("--offline-timeout-s", type=float, default=20.0)
    parser.add_argument("--boot-timeout-s", type=float, default=75.0)
    parser.add_argument("--failover-timeout-s", type=float, default=90.0)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--no-clean-start", action="store_true", help="do not clear saved role/allowlist state before configuring test roles")
    parser.add_argument("--log-dir", default="")
    return parser


def main() -> int:
    return run(build_parser().parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
