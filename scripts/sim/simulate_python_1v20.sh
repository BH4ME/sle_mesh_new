#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LOG_DIR="$ROOT_DIR/logs/sim"
LOG_FILE="$LOG_DIR/python_1v20.log"

STRESS=20
FAIL_TICK=6
RECOVER_TICK=10
TICKS=14
SHOW_TIMELINE=0
PACKET_LOSS_RATE=0.0
JITTER_MIN_MS=0
JITTER_MAX_MS=0
BATCH_FAIL_RELAY_COUNT=0
BATCH_FAIL_RELAY_TICKS=""

for arg in "$@"; do
  case "$arg" in
    --stress=*)
      STRESS="${arg#*=}"
      ;;
    --fail-tick=*)
      FAIL_TICK="${arg#*=}"
      ;;
    --recover-tick=*)
      RECOVER_TICK="${arg#*=}"
      ;;
    --ticks=*)
      TICKS="${arg#*=}"
      ;;
    --show-timeline)
      SHOW_TIMELINE=1
      ;;
    --packet-loss-rate=*)
      PACKET_LOSS_RATE="${arg#*=}"
      ;;
    --jitter-min-ms=*)
      JITTER_MIN_MS="${arg#*=}"
      ;;
    --jitter-max-ms=*)
      JITTER_MAX_MS="${arg#*=}"
      ;;
    --batch-fail-relay-count=*)
      BATCH_FAIL_RELAY_COUNT="${arg#*=}"
      ;;
    --batch-fail-relay-ticks=*)
      BATCH_FAIL_RELAY_TICKS="${arg#*=}"
      ;;
    *)
      echo "[py-sim] unknown arg: $arg" >&2
      exit 1
      ;;
  esac
done

if ! [[ "$STRESS" =~ ^[0-9]+$ ]] || [ "$STRESS" -lt 1 ]; then
  echo "[py-sim] ERROR: --stress must be a positive integer, got '$STRESS'" >&2
  exit 1
fi
if ! [[ "$FAIL_TICK" =~ ^[0-9]+$ ]] || [ "$FAIL_TICK" -lt 1 ]; then
  echo "[py-sim] ERROR: --fail-tick must be a positive integer, got '$FAIL_TICK'" >&2
  exit 1
fi
if ! [[ "$RECOVER_TICK" =~ ^[0-9]+$ ]] || [ "$RECOVER_TICK" -lt "$FAIL_TICK" ]; then
  echo "[py-sim] ERROR: --recover-tick must be >= --fail-tick, got '$RECOVER_TICK'" >&2
  exit 1
fi
if ! [[ "$TICKS" =~ ^[0-9]+$ ]] || [ "$TICKS" -lt "$RECOVER_TICK" ]; then
  echo "[py-sim] ERROR: --ticks must be >= --recover-tick, got '$TICKS'" >&2
  exit 1
fi
if ! [[ "$JITTER_MIN_MS" =~ ^[0-9]+$ ]]; then
  echo "[py-sim] ERROR: --jitter-min-ms must be >=0, got '$JITTER_MIN_MS'" >&2
  exit 1
fi
if ! [[ "$JITTER_MAX_MS" =~ ^[0-9]+$ ]]; then
  echo "[py-sim] ERROR: --jitter-max-ms must be >=0, got '$JITTER_MAX_MS'" >&2
  exit 1
fi
if [ "$JITTER_MIN_MS" -gt "$JITTER_MAX_MS" ]; then
  echo "[py-sim] ERROR: --jitter-min-ms must be <= --jitter-max-ms" >&2
  exit 1
fi
if ! [[ "$BATCH_FAIL_RELAY_COUNT" =~ ^[0-9]+$ ]]; then
  echo "[py-sim] ERROR: --batch-fail-relay-count must be >=0, got '$BATCH_FAIL_RELAY_COUNT'" >&2
  exit 1
fi

mkdir -p "$LOG_DIR"

CMD=(
  python3 "$ROOT_DIR/tools/sle_team_python_sim.py"
  --members 30
  --direct-cap 8
  --relay-fail-tick "$FAIL_TICK"
  --relay-recover-tick "$RECOVER_TICK"
  --ticks "$TICKS"
  --stress "$STRESS"
  --packet-loss-rate "$PACKET_LOSS_RATE"
  --jitter-min-ms "$JITTER_MIN_MS"
  --jitter-max-ms "$JITTER_MAX_MS"
  --batch-fail-relay-count "$BATCH_FAIL_RELAY_COUNT"
)
if [ -n "$BATCH_FAIL_RELAY_TICKS" ]; then
  CMD+=(--batch-fail-relay-ticks "$BATCH_FAIL_RELAY_TICKS")
fi
if [ "$SHOW_TIMELINE" -ne 0 ]; then
  CMD+=(--show-timeline)
fi

echo "[py-sim] running python 1vs30 simulation"
echo "[py-sim] stress=$STRESS fail_tick=$FAIL_TICK recover_tick=$RECOVER_TICK ticks=$TICKS loss=$PACKET_LOSS_RATE jitter=${JITTER_MIN_MS}-${JITTER_MAX_MS} batch_fail_count=$BATCH_FAIL_RELAY_COUNT batch_fail_ticks=${BATCH_FAIL_RELAY_TICKS:-none}"

"${CMD[@]}" | tee "$LOG_FILE"

echo "[py-sim] log: $LOG_FILE"
