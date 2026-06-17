# Branch Strategy

This repository keeps multiple WS63/SLE directions alive without forcing them
into one mainline.

## Long-lived Branches

| Branch | Purpose | Merge policy |
| --- | --- | --- |
| `main` | Stable repository index, shared docs, and known-good public entry points. | Keep clean. Do not merge lab or direction-specific experiments by default. |
| `line/v1-manual-relay` | V1 manual relay approval flow. | Maintain independently. Cherry-pick only small shared fixes when useful. |
| `line/v2-auto-networking` | V2 automatic networking and relay failover line. | Maintain independently and tag releasable alpha/beta points. |
| `line/v3-phone-location` | V3 phone geolocation bridge and SLE position distribution. | Branch from a suitable V2 point and evolve independently. |
| `lab/ws63-st7789-display` | WS63 ST7789 135x240 TFT display experiments. | Lab-only. Do not merge into V1/V2/V3 unless display becomes a product feature. |

Older temporary development branches may remain only as local history. The
`line/...`, `release/...` and `lab/...` names are the canonical working branches
going forward.

## Release Markers

Use tags to freeze points that are known to build or demonstrate a feature:

- `v1.x.y` for the V1 line.
- `v2.0.0-alphaN` for V2 automatic networking.
- `v3.0.0-alphaN` for V3 phone location work.
- `display-st7789-alphaN` for display-only lab checkpoints.

Each tagged point should document:

- What the branch demonstrates.
- How to build or flash it.
- Which validation command or board test was run.

## Daily Workflow

1. Choose the branch that matches the goal before editing.
2. Keep feature commits on that branch.
3. Push the branch directly to GitHub for backup and review.
4. Use PRs only when intentionally moving changes between branches.
5. Prefer cherry-picking small shared fixes over merging whole experimental lines.

