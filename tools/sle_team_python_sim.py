#!/usr/bin/env python3
"""Pure Python 1vs20 team-network simulator.

This simulator models a single-leader, single-tier-relay topology:
1) discovery/approve for 20 logical members under 8 direct-link cap
2) periodic position reporting
3) relay failover + auto reselection + member reparent
4) relay recovery
"""

from __future__ import annotations

from dataclasses import dataclass, field
import argparse
import random
from typing import Dict, List, Optional, Set, Tuple


@dataclass
class SimulationConfig:
    member_count: int = 20
    direct_connection_cap: int = 8
    fail_relay_at_tick: int = 6
    recover_relay_at_tick: int = 10
    ticks_total: int = 14
    relay_target: int = 3
    seed: int = 20260510
    packet_loss_rate: float = 0.0
    jitter_min_ms: int = 0
    jitter_max_ms: int = 0
    batch_fail_relay_ticks: List[int] = field(default_factory=list)
    batch_fail_relay_count: int = 0


@dataclass
class SimulationResult:
    discovered_members: int
    approved_members: int
    route_reparent_total: int
    relay_reselection_total: int
    report_success_before_failover: int
    report_success_during_failover: int
    report_success_after_recover: int
    total_report_success: int
    report_dropped: int
    report_delayed: int
    report_lost_by_parent_down: int
    batch_fail_events: int
    timeline: List[str]


@dataclass
class MemberState:
    member_id: int
    approved: bool = False
    online: bool = True
    is_relay: bool = False
    parent_id: int = 1
    rssi_to_leader: int = -80
    reports_success: int = 0


def _pick_initial_relays(members: Dict[int, MemberState], relay_target: int) -> List[int]:
    candidates = sorted(members.values(), key=lambda m: m.rssi_to_leader, reverse=True)
    picked: List[int] = []
    for m in candidates:
        if len(picked) >= relay_target:
            break
        m.is_relay = True
        m.parent_id = 1
        picked.append(m.member_id)
    return picked


def _rebalance_relays(members: Dict[int, MemberState], relay_target: int, timeline: List[str]) -> int:
    relay_ids = [m.member_id for m in members.values() if m.approved and m.online and m.is_relay]
    reselections = 0
    if len(relay_ids) >= relay_target:
        return reselections

    need = relay_target - len(relay_ids)
    candidates = sorted(
        [m for m in members.values() if m.approved and m.online and not m.is_relay],
        key=lambda m: m.rssi_to_leader,
        reverse=True,
    )
    for m in candidates[:need]:
        m.is_relay = True
        m.parent_id = 1
        reselections += 1
        timeline.append(f"relay promote member={m.member_id}")
    return reselections


def _best_available_relay(members: Dict[int, MemberState]) -> Optional[int]:
    relays = [m for m in members.values() if m.approved and m.online and m.is_relay]
    if not relays:
        return None
    relays.sort(key=lambda m: m.rssi_to_leader, reverse=True)
    return relays[0].member_id


def _route_report(m: MemberState, members: Dict[int, MemberState]) -> bool:
    if not m.online or not m.approved:
        return False
    if m.parent_id == 1:
        return True
    parent = members.get(m.parent_id)
    return bool(parent and parent.online and parent.is_relay and parent.approved)


def _available_relays(members: Dict[int, MemberState]) -> List[MemberState]:
    relays = [m for m in members.values() if m.approved and m.online and m.is_relay]
    relays.sort(key=lambda m: m.rssi_to_leader, reverse=True)
    return relays


def _on_relay_down(
    members: Dict[int, MemberState],
    down_relay_ids: List[int],
    cfg: SimulationConfig,
    timeline: List[str],
) -> Tuple[int, int]:
    route_reparent_total = 0
    relay_reselection_total = 0

    for relay_id in down_relay_ids:
        m = members[relay_id]
        m.online = False
        m.is_relay = False
        timeline.append(f"relay_fail member={relay_id}")

    for m in members.values():
        if m.member_id in down_relay_ids:
            continue
        if m.parent_id in down_relay_ids:
            old_parent = m.parent_id
            best = _best_available_relay(members)
            if best is None:
                m.parent_id = 1
                timeline.append(
                    f"leaf_reparent member={m.member_id} old_parent={old_parent} new_parent=1 mode=fallback-leader"
                )
            else:
                m.parent_id = best
                timeline.append(
                    f"leaf_reparent member={m.member_id} old_parent={old_parent} new_parent={best} mode=switch-relay"
                )
            route_reparent_total += 1

    relay_reselection_total += _rebalance_relays(members, cfg.relay_target, timeline)
    return route_reparent_total, relay_reselection_total


def simulate_one_run(cfg: SimulationConfig) -> SimulationResult:
    if cfg.jitter_min_ms > cfg.jitter_max_ms:
        raise ValueError("jitter_min_ms must be <= jitter_max_ms")
    if cfg.packet_loss_rate < 0.0 or cfg.packet_loss_rate > 1.0:
        raise ValueError("packet_loss_rate must be in [0.0, 1.0]")
    if cfg.batch_fail_relay_count < 0:
        raise ValueError("batch_fail_relay_count must be >= 0")

    rng = random.Random(cfg.seed)
    members: Dict[int, MemberState] = {}
    discovered: Set[int] = set()
    timeline: List[str] = []

    for member_id in range(2, 2 + cfg.member_count):
        members[member_id] = MemberState(
            member_id=member_id,
            approved=False,
            online=True,
            is_relay=False,
            parent_id=1,
            rssi_to_leader=-95 + ((member_id * 7) % 35),  # deterministic spread
        )

    pending_pool = list(members.keys())
    approved_count = 0

    # Pairing-discovery phase: cap-constrained rotating discovery until all found.
    while pending_pool:
        batch = pending_pool[: cfg.direct_connection_cap]
        pending_pool = pending_pool[cfg.direct_connection_cap :]
        for member_id in batch:
            discovered.add(member_id)
            m = members[member_id]
            m.approved = True
            approved_count += 1
        timeline.append(f"pairing_batch approved={len(batch)} total_approved={approved_count}")

    # Initial relay set after pairing window closed.
    relays = _pick_initial_relays(members, cfg.relay_target)
    timeline.append(f"relay_initial {relays}")

    # Leafs choose parent: by default direct leader; a subset hangs behind best relay.
    first_relay = relays[0] if relays else 1
    for m in members.values():
        if not m.is_relay and m.member_id % 3 == 0 and first_relay != 1:
            m.parent_id = first_relay

    report_before = 0
    report_during = 0
    report_after = 0
    route_reparent_total = 0
    relay_reselection_total = 0
    report_dropped = 0
    report_delayed = 0
    report_lost_by_parent_down = 0
    batch_fail_events = 0

    failed_relay_id: Optional[int] = None
    pending_deliveries: Dict[int, List[Tuple[int, int]]] = {}

    def _record_success_for_phase(send_tick_value: int, count: int) -> Tuple[int, int, int]:
        before = 0
        during = 0
        after = 0
        if count <= 0:
            return before, during, after
        if send_tick_value < cfg.fail_relay_at_tick:
            before = count
        elif send_tick_value < cfg.recover_relay_at_tick:
            during = count
        else:
            after = count
        return before, during, after

    for tick in range(1, cfg.ticks_total + 1):
        delivered_members = pending_deliveries.pop(tick, [])
        delivered = len(delivered_members)
        if delivered > 0:
            for send_tick_value, member_id in delivered_members:
                member = members.get(member_id)
                if member is not None:
                    member.reports_success += 1
            phase_totals = [0, 0, 0]
            for send_tick_value, _member_id in delivered_members:
                b, d, a = _record_success_for_phase(send_tick_value, 1)
                phase_totals[0] += b
                phase_totals[1] += d
                phase_totals[2] += a
            report_before += phase_totals[0]
            report_during += phase_totals[1]
            report_after += phase_totals[2]

        if cfg.batch_fail_relay_count > 0 and tick in cfg.batch_fail_relay_ticks:
            live_relays = _available_relays(members)
            if live_relays:
                down_count = min(cfg.batch_fail_relay_count, len(live_relays))
                down_relays = [m.member_id for m in live_relays[-down_count:]]
                timeline.append(f"batch_relay_fail tick={tick} ids={down_relays}")
                r1, r2 = _on_relay_down(members, down_relays, cfg, timeline)
                route_reparent_total += r1
                relay_reselection_total += r2
                batch_fail_events += 1

        if tick == cfg.fail_relay_at_tick:
            live_relays = _available_relays(members)
            if live_relays:
                failed_relay_id = live_relays[0].member_id
                timeline.append(f"relay_fail_tick={tick}")
                r1, r2 = _on_relay_down(members, [failed_relay_id], cfg, timeline)
                route_reparent_total += r1
                relay_reselection_total += r2

        if tick == cfg.recover_relay_at_tick and failed_relay_id is not None:
            recovering = members[failed_relay_id]
            recovering.online = True
            recovering.is_relay = False
            recovering.parent_id = 1
            timeline.append(f"relay_recover member={failed_relay_id} tick={tick}")
            relay_reselection_total += _rebalance_relays(members, cfg.relay_target, timeline)

        success_this_tick = 0
        for m in members.values():
            if not m.online or not m.approved:
                continue
            route_ok = _route_report(m, members)
            if not route_ok:
                report_lost_by_parent_down += 1
                continue
            if rng.random() < cfg.packet_loss_rate:
                report_dropped += 1
                continue

            # Jitter influences whether a report can be counted in this tick.
            if cfg.jitter_max_ms > 0:
                jitter = rng.randint(cfg.jitter_min_ms, cfg.jitter_max_ms)
                if jitter > 0:
                    report_delayed += 1
                    # Model jitter as deferred delivery to a future tick instead of loss.
                    delay_ticks = max(1, jitter // 20)
                    deliver_tick = min(cfg.ticks_total, tick + delay_ticks)
                    pending_deliveries.setdefault(deliver_tick, []).append((tick, m.member_id))
                    continue

            m.reports_success += 1
            success_this_tick += 1

        b, d, a = _record_success_for_phase(tick, success_this_tick)
        report_before += b
        report_during += d
        report_after += a

        # Minor random parent optimization to mimic periodic route update.
        if tick % 4 == 0:
            best_relay = _best_available_relay(members)
            if best_relay is not None:
                for m in members.values():
                    if m.is_relay or not m.online or not m.approved:
                        continue
                    if rng.random() < 0.08 and m.parent_id != best_relay:
                        m.parent_id = best_relay
                        route_reparent_total += 1
                        timeline.append(f"route_update member={m.member_id} parent={best_relay} tick={tick}")

    for future_tick in sorted(pending_deliveries.keys()):
        delivered_members = pending_deliveries[future_tick]
        delivered = len(delivered_members)
        if delivered <= 0:
            continue
        for send_tick_value, member_id in delivered_members:
            member = members.get(member_id)
            if member is not None:
                member.reports_success += 1
        phase_totals = [0, 0, 0]
        for send_tick_value, _member_id in delivered_members:
            b, d, a = _record_success_for_phase(send_tick_value, 1)
            phase_totals[0] += b
            phase_totals[1] += d
            phase_totals[2] += a
        report_before += phase_totals[0]
        report_during += phase_totals[1]
        report_after += phase_totals[2]

    total_success = sum(m.reports_success for m in members.values())

    return SimulationResult(
        discovered_members=len(discovered),
        approved_members=approved_count,
        route_reparent_total=route_reparent_total,
        relay_reselection_total=relay_reselection_total,
        report_success_before_failover=report_before,
        report_success_during_failover=report_during,
        report_success_after_recover=report_after,
        total_report_success=total_success,
        report_dropped=report_dropped,
        report_delayed=report_delayed,
        report_lost_by_parent_down=report_lost_by_parent_down,
        batch_fail_events=batch_fail_events,
        timeline=timeline,
    )


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Python 1vs20 team-network simulator")
    parser.add_argument("--members", type=int, default=20)
    parser.add_argument("--direct-cap", type=int, default=8)
    parser.add_argument("--relay-fail-tick", type=int, default=6)
    parser.add_argument("--relay-recover-tick", type=int, default=10)
    parser.add_argument("--ticks", type=int, default=14)
    parser.add_argument("--relay-target", type=int, default=3)
    parser.add_argument("--stress", type=int, default=1)
    parser.add_argument("--seed", type=int, default=20260510)
    parser.add_argument("--packet-loss-rate", type=float, default=0.0)
    parser.add_argument("--jitter-min-ms", type=int, default=0)
    parser.add_argument("--jitter-max-ms", type=int, default=0)
    parser.add_argument("--batch-fail-relay-count", type=int, default=0)
    parser.add_argument("--batch-fail-relay-ticks", type=str, default="")
    parser.add_argument("--show-timeline", action="store_true")
    return parser


def main() -> int:
    parser = _build_arg_parser()
    args = parser.parse_args()

    if args.members < 1 or args.direct_cap < 1 or args.ticks < 1 or args.stress < 1:
        parser.error("members/direct-cap/ticks/stress must be positive")
    if args.relay_recover_tick < args.relay_fail_tick:
        parser.error("relay-recover-tick must be >= relay-fail-tick")
    if args.packet_loss_rate < 0.0 or args.packet_loss_rate > 1.0:
        parser.error("packet-loss-rate must be in [0.0, 1.0]")
    if args.jitter_min_ms < 0 or args.jitter_max_ms < 0 or args.jitter_min_ms > args.jitter_max_ms:
        parser.error("jitter range invalid")
    if args.batch_fail_relay_count < 0:
        parser.error("batch-fail-relay-count must be >= 0")

    batch_ticks: List[int] = []
    if args.batch_fail_relay_ticks.strip():
        for raw in args.batch_fail_relay_ticks.split(","):
            raw = raw.strip()
            if not raw:
                continue
            try:
                tick = int(raw)
            except ValueError as exc:
                raise SystemExit(f"invalid batch fail tick: {raw}") from exc
            if tick < 1:
                raise SystemExit(f"batch fail tick must be >=1: {tick}")
            batch_ticks.append(tick)

    pass_runs = 0
    fail_runs = 0
    first_fail: Optional[Tuple[int, str]] = None

    for i in range(1, args.stress + 1):
        cfg = SimulationConfig(
            member_count=args.members,
            direct_connection_cap=args.direct_cap,
            fail_relay_at_tick=args.relay_fail_tick,
            recover_relay_at_tick=args.relay_recover_tick,
            ticks_total=args.ticks,
            relay_target=args.relay_target,
            seed=args.seed + i,
            packet_loss_rate=args.packet_loss_rate,
            jitter_min_ms=args.jitter_min_ms,
            jitter_max_ms=args.jitter_max_ms,
            batch_fail_relay_count=args.batch_fail_relay_count,
            batch_fail_relay_ticks=batch_ticks,
        )
        result = simulate_one_run(cfg)
        expects_reselection = False
        if 1 <= args.relay_fail_tick <= args.ticks:
            expects_reselection = True
        if args.batch_fail_relay_count > 0 and batch_ticks:
            for tick in batch_ticks:
                if 1 <= tick <= args.ticks:
                    expects_reselection = True
                    break
        ok = (
            result.discovered_members == args.members
            and result.approved_members == args.members
            and result.total_report_success > 0
            and (expects_reselection is False or result.relay_reselection_total >= 1)
        )
        if ok:
            pass_runs += 1
        else:
            fail_runs += 1
            if first_fail is None:
                first_fail = (i, f"discovered={result.discovered_members} approved={result.approved_members} "
                                 f"total_report_success={result.total_report_success} "
                                 f"relay_reselection={result.relay_reselection_total}")

        print(
            f"[py-sim] iter {i}/{args.stress} "
            f"discovered={result.discovered_members} approved={result.approved_members} "
            f"before={result.report_success_before_failover} "
            f"during={result.report_success_during_failover} "
            f"after={result.report_success_after_recover} "
            f"reparent={result.route_reparent_total} "
            f"relay_reselect={result.relay_reselection_total} "
            f"drop={result.report_dropped} "
            f"delay={result.report_delayed} "
            f"lost_parent={result.report_lost_by_parent_down} "
            f"batch_fail_events={result.batch_fail_events}"
        )
        if args.show_timeline:
            for item in result.timeline:
                print(f"  - {item}")

    print(f"[py-sim] summary pass={pass_runs} fail={fail_runs} total={args.stress}")
    if first_fail is not None:
        print(f"[py-sim] first_fail iter={first_fail[0]} detail={first_fail[1]}")
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
