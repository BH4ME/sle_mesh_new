#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
SIM_SCRIPT="$ROOT_DIR/scripts/sim/simulate_v2.sh"
LOG_DIR="$ROOT_DIR/logs/sim"
NETWORK_LOG="$LOG_DIR/network_test.log"
PACKET_LOG="$LOG_DIR/packet_test.log"
CONSOLE_LOG="$LOG_DIR/stress_20.console.log"

STRESS=200
for arg in "$@"; do
  case "$arg" in
    --stress=*)
      STRESS="${arg#*=}"
      ;;
  esac
done

if ! [[ "$STRESS" =~ ^[0-9]+$ ]] || [ "$STRESS" -lt 1 ]; then
  echo "[sim20] ERROR: --stress must be a positive integer, got '$STRESS'" >&2
  exit 1
fi

if [ ! -x "$SIM_SCRIPT" ]; then
  echo "[sim20] ERROR: missing executable script: $SIM_SCRIPT" >&2
  exit 1
fi

mkdir -p "$LOG_DIR"

assert_30_cap_present() {
  local source_file="$ROOT_DIR/examples/team_network_demo.c"
  local checks=0

  grep -q "assert(SLE_TEAM_MAX_MEMBERS >= 30U);" "$source_file" && checks=$((checks + 1)) || true
  grep -q "for (uint8_t member_id = 2U; member_id <= 31U; member_id++)" "$source_file" && checks=$((checks + 1)) || true
  grep -q "assert(leader.cfg.allowed_member_count == 30U);" "$source_file" && checks=$((checks + 1)) || true

  if [ "$checks" -ne 3 ]; then
    echo "[sim20] ERROR: 30-member assertions not fully present in team_network_demo.c (found $checks/3)" >&2
    exit 1
  fi
}

count_or_zero() {
  local pattern="$1"
  local file="$2"
  if [ ! -f "$file" ]; then
    echo 0
    return
  fi
  grep -c "$pattern" "$file" || true
}

assert_30_cap_present

echo "[sim20] running dedicated 30-member simulation"
echo "[sim20] stress iterations: $STRESS"

"$SIM_SCRIPT" --stress="$STRESS" > "$CONSOLE_LOG" 2>&1

summary_line="$(grep "\[sim\] summary:" "$CONSOLE_LOG" | tail -n 1)"
if [ -z "$summary_line" ]; then
  echo "[sim20] ERROR: missing summary line from simulation output" >&2
  echo "[sim20] console log: $CONSOLE_LOG" >&2
  exit 1
fi

pass_count="$(echo "$summary_line" | sed -n 's/.*pass=\([0-9][0-9]*\).*/\1/p')"
fail_count="$(echo "$summary_line" | sed -n 's/.*fail=\([0-9][0-9]*\).*/\1/p')"
total_count="$(echo "$summary_line" | sed -n 's/.*total=\([0-9][0-9]*\).*/\1/p')"

pending_count="$(count_or_zero "member pending approval" "$NETWORK_LOG")"
approved_count="$(count_or_zero "member approved" "$NETWORK_LOG")"
retry_count="$(count_or_zero "pairing stopped with pending retry" "$NETWORK_LOG")"
relay_forwarded_count="$(count_or_zero "relay forwarded packet" "$NETWORK_LOG")"
leader_timeout_count="$(count_or_zero "leader timeout, rejoining" "$NETWORK_LOG")"
config_reject_count="$(count_or_zero "config rejected by role/source" "$NETWORK_LOG")"

printf '[sim20] summary: pass=%s fail=%s total=%s\n' "$pass_count" "$fail_count" "$total_count"
printf '[sim20] metrics: pending=%s approved=%s retry=%s relay_forwarded=%s leader_timeout=%s config_reject=%s\n' \
  "$pending_count" "$approved_count" "$retry_count" "$relay_forwarded_count" "$leader_timeout_count" "$config_reject_count"

echo "[sim20] logs:"
echo "[sim20] console: $CONSOLE_LOG"
echo "[sim20] network: $NETWORK_LOG"
echo "[sim20] packet : $PACKET_LOG"
