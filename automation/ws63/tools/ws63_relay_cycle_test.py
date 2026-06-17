#!/usr/bin/env python3
"""WS63 three-board relay failover serial test.

Validates the live relay requirement with three boards:
leader + relay member + child member. The relay is rebooted to simulate
signal loss, not manual leave. The child must reselect an upstream parent and
continue/recover communication with the leader.
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
    from automation.ws63.tools import ws63_link_cycle_test as lc
except ModuleNotFoundError:  # pragma: no cover - direct script execution
    repo_root = pathlib.Path(__file__).resolve().parents[3]
    sys.path.insert(0, str(repo_root))
    from automation.ws63.tools import ws63_link_cycle_test as lc


MemberRecord = dict[str, int]


def _progress(message: str) -> None:
    print(f"[relay-cycle] {message}", flush=True)


def _send_cli_line(peer: lc.Peer, line: str) -> None:
    peer.ser.write(b"\r\n")
    peer.ser.flush()
    time.sleep(0.03)
    peer.send_line(line)


def _clear_peer_rx(peer: lc.Peer) -> None:
    try:
        peer.ser.reset_input_buffer()
    except Exception:  # noqa: BLE001
        pass


def _drain_to_text(target: lc.Peer, peers: Iterable[lc.Peer], seconds: float) -> str:
    end = time.time() + seconds
    text = ""
    while time.time() < end:
        for peer in peers:
            chunk = lc._read_once(peer)
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


def _latest_cfg_json(text: str) -> Optional[dict[str, object]]:
    latest: Optional[dict[str, object]] = None
    for match in re.finditer(r"\[cfg-json\]\s*(\{[^\r\n]*\})", text):
        try:
            latest = json.loads(match.group(1))
        except json.JSONDecodeError:
            continue
    return latest


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
        time.sleep(0.3)
    tail = "".join(peer.log)[-1000:].replace("\r", "\\r").replace("\n", "\\n")
    raise RuntimeError(f"{peer.name} cfg status did not return cfg-json; last_rx={len(last_text)} tail={tail}")


def _assert_fw(peers: Iterable[lc.Peer], all_peers: Iterable[lc.Peer], expected_fw: str) -> dict[str, dict[str, object]]:
    out: dict[str, dict[str, object]] = {}
    for peer in peers:
        status = _query_cfg(peer, all_peers)
        fw = status.get("fw")
        if expected_fw and fw != expected_fw:
            raise RuntimeError(f"{peer.name} firmware mismatch: expected {expected_fw}, got {fw}")
        out[peer.name] = status
        _progress(f"{peer.name} {peer.port} fw={fw} suffix={status.get('selfSuffix')} route={status.get('routeId')}")
    return out


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


def _send_cfg_and_wait(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    command: str,
    pattern: str,
    timeout_s: float,
    note: str,
) -> None:
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
            if rx.search(text):
                return
            for item in peers:
                lc._read_once(item)
            time.sleep(0.02)
    raise RuntimeError(f"timeout waiting for {note}: pattern={pattern}")


def _parse_member_records(text: str) -> dict[int, MemberRecord]:
    records: dict[int, MemberRecord] = {}
    for line in text.splitlines():
        member_match = re.search(r"member=(\d+)", line)
        if not member_match:
            continue
        member_id = int(member_match.group(1))

        def _field_int(name: str) -> Optional[int]:
            match = re.search(rf"\b{name}=(-?\d+)", line)
            if match is None:
                return None
            return int(match.group(1))

        online = _field_int("online")
        relay = _field_int("relay")
        tier = _field_int("tier")
        max_down = _field_int("max_down")
        last_seen = _field_int("last_seen")
        if None in (online, relay, tier, max_down, last_seen):
            continue

        record: MemberRecord = {
            "online": int(online),
            "relay": int(relay),
            "tier": int(tier),
            "max_down": int(max_down),
            "last_seen": int(last_seen),
        }
        parent_id = _field_int("parent")
        next_hop = _field_int("next")
        child_count = _field_int("child_count")
        rssi = _field_int("rssi")
        ready = _field_int("ready")
        if parent_id is not None:
            record["parent_id"] = int(parent_id)
        if next_hop is not None:
            record["next_hop"] = int(next_hop)
        if child_count is not None:
            record["child_count"] = int(child_count)
        if rssi is not None:
            record["rssi"] = int(rssi)
        if ready is not None:
            record["mac_ready"] = int(ready)
        mac_match = re.search(r"\bmac=([0-9A-Fa-f]{4})\b", line)
        if mac_match is not None:
            record["mac_suffix"] = int(mac_match.group(1), 16)
        records[member_id] = record
    return records


def _query_member_record(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    member_id: int,
    query_window_s: float,
) -> Optional[MemberRecord]:
    leader.send_line("members")
    end = time.time() + query_window_s
    buf = ""
    while time.time() < end:
        for peer in peers:
            chunk = lc._read_once(peer)
            if peer is leader and chunk:
                buf += chunk
        time.sleep(0.02)
    return _parse_member_records(buf).get(member_id)


def _wait_member_record(
    leader: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    expect_online: Optional[int],
    expect_relay: Optional[int],
    timeout_s: float,
    poll_s: float,
    note: str,
) -> MemberRecord:
    end = time.time() + timeout_s
    last: Optional[MemberRecord] = None
    while time.time() < end:
        record = _query_member_record(leader, peers, member_id, query_window_s=min(0.6, poll_s))
        if record is not None:
            last = record
            online_ok = expect_online is None or record["online"] == expect_online
            relay_ok = expect_relay is None or record["relay"] == expect_relay
            if online_ok and relay_ok:
                return record
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"timeout waiting for {note}: member={member_id} last={last}")


def _query_joined(peer: lc.Peer, peers: Iterable[lc.Peer], query_window_s: float) -> Optional[int]:
    peer.send_line("state")
    end = time.time() + query_window_s
    buf = ""
    while time.time() < end:
        for item in peers:
            chunk = lc._read_once(item)
            if item is peer and chunk:
                buf += chunk
        time.sleep(0.02)

    joined: Optional[int] = None
    for match in re.finditer(r"\bjoined=(\d+)\b", buf):
        joined = int(match.group(1))
    return joined


def _wait_peer_joined(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    timeout_s: float,
    poll_s: float,
    note: str,
) -> None:
    end = time.time() + timeout_s
    while time.time() < end:
        joined = _query_joined(peer, peers, query_window_s=min(0.6, poll_s))
        if joined == 1:
            return
        time.sleep(max(0.1, poll_s - 0.6))
    raise RuntimeError(f"timeout waiting for {note}: joined=1")


def _wait_log_pattern(
    target: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    pattern: str,
    timeout_s: float,
    note: str,
    log_start: int = 0,
) -> None:
    rx = re.compile(pattern)
    end = time.time() + timeout_s
    while time.time() < end:
        if rx.search("".join(target.log[log_start:])):
            return
        for peer in peers:
            lc._read_once(peer)
        time.sleep(0.02)
    raise RuntimeError(f"timeout waiting for {note}: pattern={pattern}")


def _collect_leader_visibility(
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
    pattern = re.compile(rf"(pending member={member_id}\b|HELLO {member_id}->{leader_id}\b)")
    while time.time() < end:
        if pattern.search("".join(leader.log[start:])):
            return
        record = _query_member_record(leader, peers, member_id, query_window_s=min(0.4, poll_s))
        if record is not None:
            return
        leader.send_line("pairing pending")
        wait_end = time.time() + min(0.4, poll_s)
        while time.time() < wait_end:
            for peer in peers:
                lc._read_once(peer)
            time.sleep(0.02)
    raise RuntimeError(f"leader did not see member={member_id} pending/hello before approval")


def _configure_member(
    leader: lc.Peer,
    member: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    member_id: int,
    leader_suffix: int,
    leader_id: int,
    team_id: int,
    channel: int,
    approve_relay: int,
    bootstrap_roles: bool,
    cfg_runtime_roles: bool,
    cmd_timeout_s: float,
    state_timeout_s: float,
    poll_interval_s: float,
    note: str,
) -> None:
    if cfg_runtime_roles:
        _progress(f"configure {note}: cfg member now {leader_suffix:04X} {team_id} {channel}")
        _send_cfg_and_wait(
            member,
            peers,
            command=f"cfg member now {leader_suffix:04X} {team_id} {channel}",
            pattern=rf"member-now queued ret=0 leader_suffix={leader_suffix:04X}",
            timeout_s=cmd_timeout_s,
            note=f"{note} cfg member now",
        )
        _wait_cfg_field(
            member,
            peers,
            key="runtimeRole",
            expected="member",
            timeout_s=state_timeout_s,
            note=f"{note} runtime member",
        )
    elif bootstrap_roles:
        member.send_line(f"role member {leader_suffix:04X}")
        lc._wait_regex(member, peers, r"role member leader_suffix=[0-9A-Fa-f]{4} .*ret=0", cmd_timeout_s,
                       f"{note} role member")
        lc._wait_role(member, peers, expected_role=0, timeout_s=state_timeout_s, poll_s=poll_interval_s)
    else:
        member.send_line(f"join {team_id} {leader_id} {channel}")
        lc._wait_regex(member, peers, r"join team=.* ret=0", cmd_timeout_s, f"{note} join")

    _collect_leader_visibility(
        leader,
        peers,
        member_id=member_id,
        leader_id=leader_id,
        timeout_s=state_timeout_s,
        poll_s=poll_interval_s,
    )
    approve_command = f"pairing approve {member_id} {'relay' if approve_relay else 'norelay'}"
    approve_pattern = rf"pairing approve member={member_id} relay={approve_relay} ret=0"
    if cfg_runtime_roles:
        _send_cfg_and_wait(
            leader,
            peers,
            command=approve_command,
            pattern=approve_pattern,
            timeout_s=cmd_timeout_s,
            note=f"{note} approve",
        )
    else:
        leader.send_line(approve_command)
        lc._wait_regex(
            leader,
            peers,
            approve_pattern,
            cmd_timeout_s,
            f"{note} approve",
        )
    _wait_member_record(
        leader,
        peers,
        member_id=member_id,
        expect_online=1,
        expect_relay=approve_relay,
        timeout_s=state_timeout_s,
        poll_s=poll_interval_s,
        note=f"{note} leader member table",
    )
    _wait_peer_joined(member, peers, timeout_s=state_timeout_s, poll_s=poll_interval_s, note=f"{note} joined")


def _detect_route_id(
    peer: lc.Peer,
    peers: Iterable[lc.Peer],
    *,
    provided_id: int,
    timeout_s: float,
    note: str,
) -> int:
    if provided_id > 0:
        return provided_id
    suffix = lc._detect_suffix(peer, peers, "", timeout_s, note)
    return lc._route_id_from_suffix(suffix)


def _dump_logs(peers: Iterable[lc.Peer], out_dir: pathlib.Path) -> None:
    out_dir.mkdir(parents=True, exist_ok=True)
    for peer in peers:
        path = out_dir / f"{peer.name}_{pathlib.Path(peer.port).name}.log"
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
    try:
        leader = lc._open_peer("leader", args.leader_port, args.baudrate)
        relay = lc._open_peer("relay", args.relay_port, args.baudrate)
        child = lc._open_peer("child", args.child_port, args.baudrate)
        peers = [leader, relay, child]

        lc._drain(peers, args.initial_drain_s)

        if args.cfg_runtime_roles or args.expected_fw:
            statuses = _assert_fw(peers, peers, args.expected_fw)
            leader_suffix_text = args.leader_suffix or str(statuses["leader"]["selfSuffix"])
            leader_suffix = int(leader_suffix_text, 16)
            leader_id = args.leader_id if args.leader_id > 0 else int(statuses["leader"]["routeId"])
            relay_id = args.relay_id if args.relay_id > 0 else int(statuses["relay"]["routeId"])
            child_id = args.child_id if args.child_id > 0 else int(statuses["child"]["routeId"])
        else:
            leader_suffix = lc._detect_suffix(
                leader,
                peers,
                args.leader_suffix,
                args.bootstrap_timeout_s,
                "leader wifi status",
            )
            leader_id = args.leader_id if args.leader_id > 0 else lc._route_id_from_suffix(leader_suffix)
            relay_id = _detect_route_id(relay, peers, provided_id=args.relay_id,
                                        timeout_s=args.bootstrap_timeout_s, note="relay wifi status")
            child_id = _detect_route_id(child, peers, provided_id=args.child_id,
                                        timeout_s=args.bootstrap_timeout_s, note="child wifi status")
        if len({leader_id, relay_id, child_id}) != 3:
            raise RuntimeError(f"route ids must be unique: leader={leader_id} relay={relay_id} child={child_id}")

        if args.cfg_runtime_roles:
            _progress(f"configure leader: cfg leader now {args.team_id} {args.channel}")
            _send_cfg_and_wait(
                leader,
                peers,
                command=f"cfg leader now {args.team_id} {args.channel}",
                pattern=rf"leader-now queued ret=0 team={args.team_id} channel={args.channel}",
                timeout_s=args.cmd_timeout_s,
                note="leader cfg now",
            )
            _wait_cfg_field(
                leader,
                peers,
                key="runtimeRole",
                expected="leader",
                timeout_s=args.state_timeout_s,
                note="leader runtime",
            )
            _progress(f"configure leader direct cap: cfg direct {args.direct_cap}")
            _send_cfg_and_wait(
                leader,
                peers,
                command=f"cfg direct {args.direct_cap}",
                pattern=rf"\[cfg\] direct cap={args.direct_cap}\b.*ret=0",
                timeout_s=args.cmd_timeout_s,
                note="leader direct cap",
            )
        elif args.bootstrap_roles:
            leader.send_line("role leader")
            lc._wait_regex(leader, peers, r"role leader ret=0", args.bootstrap_timeout_s, "set leader role")
            lc._wait_role(leader, peers, expected_role=1, timeout_s=args.bootstrap_timeout_s,
                          poll_s=args.poll_interval_s)

        pairing_start_pattern = r"(pairing started|pairing start ret=0|leader force rescan reason=pairing_window)"
        if args.cfg_runtime_roles:
            _send_cfg_and_wait(
                leader,
                peers,
                command="pairing start",
                pattern=pairing_start_pattern,
                timeout_s=args.cmd_timeout_s,
                note="pairing start",
            )
        else:
            leader.send_line("pairing start")
            lc._wait_regex(leader, peers, pairing_start_pattern, args.cmd_timeout_s, "pairing start")

        _configure_member(
            leader,
            relay,
            peers,
            member_id=relay_id,
            leader_suffix=leader_suffix,
            leader_id=leader_id,
            team_id=args.team_id,
            channel=args.channel,
            approve_relay=1,
            bootstrap_roles=args.bootstrap_roles,
            cfg_runtime_roles=args.cfg_runtime_roles,
            cmd_timeout_s=args.cmd_timeout_s,
            state_timeout_s=args.state_timeout_s,
            poll_interval_s=args.poll_interval_s,
            note="relay",
        )

        child_parent_log_start = len(child.log)
        _configure_member(
            leader,
            child,
            peers,
            member_id=child_id,
            leader_suffix=leader_suffix,
            leader_id=leader_id,
            team_id=args.team_id,
            channel=args.channel,
            approve_relay=0,
            bootstrap_roles=args.bootstrap_roles,
            cfg_runtime_roles=args.cfg_runtime_roles,
            cmd_timeout_s=args.cmd_timeout_s,
            state_timeout_s=args.state_timeout_s,
            poll_interval_s=args.poll_interval_s,
            note="child",
        )

        if args.require_child_parent_relay:
            _wait_log_pattern(
                child,
                peers,
                pattern=rf"upstream parent={relay_id}\b.*state=",
                timeout_s=args.parent_timeout_s,
                note="child initially uses relay as upstream parent",
                log_start=child_parent_log_start,
            )

        pairing_stop_pattern = r"(pairing stopped|pairing stop ret=0)"
        if args.cfg_runtime_roles:
            _send_cfg_and_wait(
                leader,
                peers,
                command="pairing stop",
                pattern=pairing_stop_pattern,
                timeout_s=args.cmd_timeout_s,
                note="pairing stop",
            )
        else:
            leader.send_line("pairing stop")
            lc._wait_regex(leader, peers, pairing_stop_pattern, args.cmd_timeout_s, "pairing stop")

        leader_offline_log_start = len(leader.log)
        child_failover_log_start = len(child.log)
        relay.send_line(args.relay_reboot_command)
        lc._wait_leader_offline_event(
            leader,
            peers,
            member_id=relay_id,
            timeout_s=args.relay_offline_timeout_s,
            note="leader offline after relay reboot",
            log_start=leader_offline_log_start,
        )
        _wait_log_pattern(
            child,
            peers,
            pattern=rf"(upstream parent reselect parent={relay_id}\b|upstream parent=(?!{relay_id}\b)\d+\b.*state=|joined member={child_id}\b)",
            timeout_s=args.failover_timeout_s,
            note="child reselects upstream parent after relay signal loss",
            log_start=child_failover_log_start,
        )
        _wait_peer_joined(child, peers, timeout_s=args.failover_timeout_s,
                          poll_s=args.poll_interval_s, note="child remains/rejoins team after relay loss")
        _wait_member_record(
            leader,
            peers,
            member_id=child_id,
            expect_online=1,
            expect_relay=None,
            timeout_s=args.failover_timeout_s,
            poll_s=args.poll_interval_s,
            note="leader sees child online after relay loss",
        )

        if not args.skip_pos_report:
            child.send_line(f"pos {leader_id} 0 0 0 0 88 1 0")
            _wait_log_pattern(
                leader,
                peers,
                pattern=rf"POS_REPORT {child_id}->{leader_id}\b",
                timeout_s=args.failover_timeout_s,
                note="leader receives child POS_REPORT after relay loss",
                log_start=leader_offline_log_start,
            )

        lc._wait_regex(
            relay,
            peers,
            lc.MEMBER_RESTORE_PATTERN,
            args.relay_boot_timeout_s,
            "relay restore from NV after reboot",
        )
        _wait_member_record(
            leader,
            peers,
            member_id=relay_id,
            expect_online=1,
            expect_relay=None,
            timeout_s=args.state_timeout_s,
            poll_s=args.poll_interval_s,
            note="relay rejoins after reboot",
        )

        print(
            "[relay-cycle] PASS: relay reboot/loss recovered child route "
            f"leader={leader_id} relay={relay_id} child={child_id}"
        )
        return 0
    except Exception as exc:  # noqa: BLE001
        print(f"[relay-cycle] FAIL: {exc}")
        return 1
    finally:
        if args.log_dir:
            _dump_logs(peers, pathlib.Path(args.log_dir))
        for peer in peers:
            try:
                peer.ser.close()
            except Exception:  # noqa: BLE001
                pass


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="WS63 live three-board relay failover test")
    parser.add_argument("--leader-port", required=True, help="leader serial port")
    parser.add_argument("--relay-port", required=True, help="relay member serial port")
    parser.add_argument("--child-port", required=True, help="child member serial port")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--expected-fw", default="")
    parser.add_argument("--cfg-runtime-roles", action="store_true", help="use cfg leader/member now runtime config")
    parser.add_argument("--bootstrap-roles", action="store_true", help="configure leader/member roles before test")
    parser.add_argument("--leader-suffix", default="", help="optional leader MAC suffix hex")
    parser.add_argument("--team-id", type=int, default=1)
    parser.add_argument("--leader-id", type=int, default=-1)
    parser.add_argument("--relay-id", type=int, default=-1)
    parser.add_argument("--child-id", type=int, default=-1)
    parser.add_argument("--channel", type=int, default=17)
    parser.add_argument("--direct-cap", type=int, default=1)
    parser.add_argument("--relay-reboot-command", default="reboot")
    parser.add_argument("--initial-drain-s", type=float, default=1.0)
    parser.add_argument("--bootstrap-timeout-s", type=float, default=20.0)
    parser.add_argument("--cmd-timeout-s", type=float, default=8.0)
    parser.add_argument("--state-timeout-s", type=float, default=30.0)
    parser.add_argument("--parent-timeout-s", type=float, default=30.0)
    parser.add_argument("--relay-offline-timeout-s", type=float, default=30.0)
    parser.add_argument("--relay-boot-timeout-s", type=float, default=60.0)
    parser.add_argument("--failover-timeout-s", type=float, default=60.0)
    parser.add_argument("--poll-interval-s", type=float, default=1.0)
    parser.add_argument("--no-require-child-parent-relay", dest="require_child_parent_relay", action="store_false")
    parser.add_argument("--skip-pos-report", action="store_true")
    parser.add_argument("--log-dir", default="")
    parser.set_defaults(require_child_parent_relay=True)
    return parser


def main() -> int:
    parser = build_parser()
    args = parser.parse_args()
    return run(args)


if __name__ == "__main__":
    raise SystemExit(main())
