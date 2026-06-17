# WS63 Minimal Networking Rewrite Task Book

## Decision

Stop patching the existing staged/frozen topology networking code. Delete the old networking strategy layer and rewrite the smallest usable leader/member/relay logic from a blank design.

This task book replaces the previous staged rewrite plan. The old plan is no longer the target.

## What "Delete All Networking" Means

Delete the old group-networking algorithm and its patch chain:

- staged leader states such as `DISCOVERING`, `FREEZING`, `PROVISIONING`, and `STABLE`;
- frozen topology planning;
- topology policy queues;
- stable rejoin leases;
- failover windows;
- route hint caches;
- provisional physical peer authorization tricks;
- relay auto-promote/rebalance/swap logic;
- special HELLO/JOINED/temporary-direct patch paths.

Keep only the non-negotiable firmware base needed to build and test:

- SLE UART client/server transport drivers;
- packet encode/decode helpers;
- UART CLI, status output, build, flash, and readback tools;
- board identity, version, and anti-pollution cleanup.

If a retained file still contains old networking decisions, remove those decisions from that file.

## Minimal Target

The first rewrite target is not clever. It is a small deterministic network:

1. Leader discovers members.
2. Leader assigns each member exactly one parent:
   - parent is the leader while stable direct slots are available;
   - otherwise parent is one currently online leader-direct relay.
3. Member accepts only the latest leader policy.
4. Relay forwards packets between its children and the leader. It does not choose topology.
5. Heartbeat timeout marks a member offline.
6. A rejoining member is treated as fresh and receives a new parent assignment.

Stable-state rule: no global regrouping, no periodic rebalance, and no parent movement just to improve topology.

Loss-handling rule: real natural loss is not stable-state rebalance. Single-member loss, double-member loss, and relay loss must recover through fresh leader policy without shrinking the intended member count.

No stale parent or relay role may be reused unless the leader sends it again.

## Hard Limits

- Leader stable direct capacity is `direct_cap=7`.
- WS63 physical max is 8 links; one physical slot may be used as a temporary admission link.
- Do not reduce the desired member count to make tests pass.
- Natural loss means reboot, power loss, or distance/signal loss, not active `leave`.
- Failed-burn, wrong-version, or no-status boards must not join validation. They must be idle/unconfigured or excluded.
- Single-member and double-member natural loss must recover without shrinking the intended member set.
- Every fresh connection or reconnection must be treated as a policy event: the leader recalculates whether that node should be direct or relayed and sends a fresh assignment.
- In stable state, do not rebalance, regroup, or change an online member's parent just to improve topology. Parent changes are driven only by fresh join/rejoin or a real loss event.
- A relay that naturally disappears is no longer trusted as a relay when it comes back. On rejoin it is handled as a normal member and can receive whatever fresh leader policy currently fits.
- When a relay naturally disappears and its children still need service, the group must recover by admitting/selecting one replacement relay from affected children or available members under leader policy, using the spare physical slot when needed.
- When the original relay later comes back, it should first rejoin as a normal child/member under the replacement relay if that is the current valid topology. It must not automatically reclaim relay status just because it used to be a relay.
- Relay soft reboot, relay natural loss, and stable-state rebalance are three different cases. Soft reboot may restore the original relay quickly; natural relay loss must prove children recover while the original relay is absent.

## Minimal State

### Leader member table

Each member has only the fields needed for fresh policy, liveness, forwarding, and relay-loss recovery:

- `id`;
- `online`;
- `policy_pending`;
- `pending_ack_seq`;
- `parent_id`;
- `relay_allowed`;
- `last_seen_s`;
- `conn_id` if direct to leader;
- `child_count` if it is a relay.
- `relay_recovery_candidate` when the member was affected by a relay loss and may be selected as replacement.

The leader also keeps a small relay-loss recovery marker:

- `relay_recovery_pending`;
- `relay_recovery_lost_relay_id`;
- `relay_recovery_selected_id`.

### Member state

Each member has only:

- `leader_id`;
- `parent_id`;
- `joined`;
- `last_leader_seen_s`;
- `upstream_conn_id`;
- `relay_allowed`.
- `relay_discovery_only` only if needed to keep recovery discovery active without treating the node as fully joined.

### Route state

Route state is not a topology engine. It is only a forwarding table:

- direct member id to leader connection;
- child member id to relay connection;
- relay upstream connection to leader.

No inferred route may change parent policy. Only leader policy changes parent policy.

## Messages

Keep the packet vocabulary small:

- `HELLO`: member announces itself.
- `POLICY`: leader assigns `parent_id` and `relay_allowed`. In the current packet set this may be represented by `ROUTE_UPDATE` plus `CONFIG`.
- `ACK`: member confirms policy.
- `HEARTBEAT`: liveness.
- `DATA`: optional payload/position/status after join.

Existing packet structs may be reused if that keeps the build small, but old message semantics must not carry hidden topology behavior.

## Leader Algorithm

On `HELLO` or rejoin:

1. Update or create the member row.
2. Clear stale role assumptions from the previous incarnation of that member unless the fresh policy explicitly grants them again.
3. If direct leader slots are available, assign parent `leader`.
4. Else choose the best online leader-direct relay that still has child capacity.
5. "Best relay" is evaluated first by usable capacity, then by link quality evidence when available, then by lowest child count as the deterministic tie breaker.
6. Send `POLICY`.
7. Do not mark the member fully online until `ACK` or valid heartbeat arrives through the assigned path.

On heartbeat timeout:

1. Mark member offline.
2. If the lost member was not a relay, do not move healthy online members. The lost member rejoins later through fresh policy.
3. If the lost member was a relay, mark the relay and its children offline, then open a relay-loss recovery window.
4. During relay-loss recovery, admit or select exactly one replacement relay candidate through the spare physical slot. Prefer an affected child with recent liveness/RSSI evidence; otherwise use deterministic lowest child count / lowest id tie-breakers.
5. The replacement relay is not fully online until ACK or heartbeat confirms the fresh policy.
6. After replacement relay confirmation, affected children rejoin through fresh leader policy and attach under the replacement relay when direct capacity is full.
7. The original relay, if it returns after replacement, is handled as a normal member/child first. It must not reclaim relay status unless the leader later grants a fresh relay policy.

On stable online members:

1. Keep sending heartbeat/config only as needed for liveness.
2. Do not move a member between direct and relay parents while it remains healthy.
3. Do not promote/demote relays except when satisfying explicit relay target, handling a join/rejoin, or handling a real loss event.

## Member Algorithm

On boot/config:

1. Start unjoined.
2. Send `HELLO` until a leader `POLICY` arrives.
3. Connect to the assigned parent.
4. Send `ACK`.
5. Send heartbeat only through that parent.

On parent loss:

1. Clear `joined`.
2. Clear local parent connection.
3. Return to `HELLO` loop and wait for fresh leader policy.
4. Keep recovery discovery active so the node can hear leader/relay policy and, if selected, become the replacement relay. Do not pause scanning in a way that prevents recovery.

## Relay Algorithm

A relay is just a member with `relay_allowed=1`:

1. Maintain upstream connection to leader.
2. Accept children only when `relay_allowed=1` and child count is below capacity.
3. Forward child packets upstream.
4. Forward leader packets to known children.
5. Never assign another member's parent.
6. Never promote itself or another node.

## Code Rewrite Boundary

### Delete or replace

- Old networking code in `xc/ws63_team_network/src/ws63_team_network_app.c`.
- Old topology/rejoin/failover tests that assert the old staged design.
- Build guards that require old staged/frozen strings.
- Version notes that describe the staged rewrite as current target.

### Keep or simplify

- `src/sle_team_packet.c` and `include/sle_team_packet.h`, unless a smaller packet set is easier.
- `src/sle_team_node.c` only if it is reduced to the minimal leader/member state machine above.
- SLE UART client/server drivers as transport, with old policy decisions removed from the app layer.
- Flash/readback/cleanup automation.

## Implementation Phases

### Phase 1 - Strip old strategy

- Remove old staged/frozen topology requirements from docs and tests.
- Add tests that fail if old topology symbols remain in the active app source.
- Reduce the active app source to transport, CLI/status, identity, and one minimal networking loop.

### Phase 2 - Direct-only bring-up

- Leader plus up to `direct_cap=7` direct members.
- Validate join, heartbeat, natural reboot rejoin.
- No relay required in this phase.

### Phase 3 - Minimal relay

- Assign overflow members to existing leader-direct relays.
- Relay forwards only.
- Validate full member count can join without direct-cap overrun.

### Phase 4 - Natural loss

- Single member reboot recovery.
- Two member reboot recovery, sequenced through the spare physical link if needed.
- Rejoining members must receive a new direct/relay policy from the leader.

### Phase 5 - Relay natural-loss recovery

- Relay loss marks the relay and affected children offline, then opens a recovery window.
- One replacement relay is selected/admitted under leader policy through the spare physical slot.
- Affected children recover by fresh rejoin policy through the replacement relay.
- A recovered former relay rejoins as a normal member/child unless the leader explicitly grants relay again.
- Do not solve relay-loss recovery by reducing the number of intended members.

## Acceptance Criteria

- Source no longer contains the old staged/frozen topology strategy.
- Firmware builds with the minimal network logic.
- Burn/readback confirms the expected version.
- Direct-only join/reboot works first.
- Then relay overflow join works.
- Then single and double natural member reboot recovery work.
- Then relay natural loss recovers children through a replacement relay while the original relay is absent.
- The task is not complete until a hardware or strong simulation record proves single natural loss and double natural loss recover with the intended member count preserved.
- The task is not complete until relay natural loss recovery is proven separately from relay soft reboot.
- The task is not complete until wrong-version/no-status/burn-failed boards are shown idle or excluded before networking validation.
- Relay soft reboot, relay natural loss, and stable-state rebalancing are distinct tests and must not be treated as one proof.

Do not claim "networking is normal" until hardware logs prove the relevant phase.
