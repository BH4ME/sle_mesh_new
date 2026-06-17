#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/../.." && pwd)"
LOG_DIR="$ROOT_DIR/logs/sim"
BUILD_DIR="${TMPDIR:-/tmp}/sle_team_sim"
NETWORK_BIN="$BUILD_DIR/sle_team_network_test"
PACKET_BIN="$BUILD_DIR/sle_team_packet_test"
REGRESSION_BIN="$BUILD_DIR/sle_team_node_regression_test"
REBALANCE_BIN="$BUILD_DIR/sle_team_relay_rebalance_test"
FAILOVER_BIN="$BUILD_DIR/sle_team_failover_suite_test"
PYTHON_SIM_SCRIPT="$ROOT_DIR/tools/sle_team_python_sim.py"
PYTHON_BIN="${PYTHON_BIN:-python3}"
NETWORK_LOG="$LOG_DIR/network_test.log"
PACKET_LOG="$LOG_DIR/packet_test.log"
REGRESSION_LOG="$LOG_DIR/team_node_regression_test.log"
REBALANCE_LOG="$LOG_DIR/relay_rebalance_test.log"
FAILOVER_LOG="$LOG_DIR/failover_suite.log"
PYTHON_LOG=""

ITERATIONS=1
SUITE="all"
PY_MEMBERS=20
PY_DIRECT_CAP=8
PY_FAIL_TICK=6
PY_RECOVER_TICK=10
PY_TICKS=14
PY_RELAY_TARGET=3
PY_SEED=20260510
PY_PACKET_LOSS_RATE="0.0"
PY_JITTER_MIN_MS=0
PY_JITTER_MAX_MS=0
PY_BATCH_FAIL_RELAY_COUNT=0
PY_BATCH_FAIL_RELAY_TICKS=""
PY_SHOW_TIMELINE=0
for arg in "$@"; do
  case "$arg" in
    --stress=*)
      ITERATIONS="${arg#*=}"
      ;;
    --suite=*)
      SUITE="${arg#*=}"
      ;;
    --py-members=*)
      PY_MEMBERS="${arg#*=}"
      ;;
    --py-direct-cap=*)
      PY_DIRECT_CAP="${arg#*=}"
      ;;
    --py-fail-tick=*)
      PY_FAIL_TICK="${arg#*=}"
      ;;
    --py-recover-tick=*)
      PY_RECOVER_TICK="${arg#*=}"
      ;;
    --py-ticks=*)
      PY_TICKS="${arg#*=}"
      ;;
    --py-relay-target=*)
      PY_RELAY_TARGET="${arg#*=}"
      ;;
    --py-seed=*)
      PY_SEED="${arg#*=}"
      ;;
    --py-packet-loss-rate=*)
      PY_PACKET_LOSS_RATE="${arg#*=}"
      ;;
    --py-jitter-min-ms=*)
      PY_JITTER_MIN_MS="${arg#*=}"
      ;;
    --py-jitter-max-ms=*)
      PY_JITTER_MAX_MS="${arg#*=}"
      ;;
    --py-batch-fail-relay-count=*)
      PY_BATCH_FAIL_RELAY_COUNT="${arg#*=}"
      ;;
    --py-batch-fail-relay-ticks=*)
      PY_BATCH_FAIL_RELAY_TICKS="${arg#*=}"
      ;;
    --py-show-timeline)
      PY_SHOW_TIMELINE=1
      ;;
  esac
done

if ! [[ "$ITERATIONS" =~ ^[0-9]+$ ]] || [ "$ITERATIONS" -lt 1 ]; then
  echo "[sim] ERROR: --stress must be a positive integer, got '$ITERATIONS'" >&2
  exit 1
fi
if ! [[ "$PY_MEMBERS" =~ ^[0-9]+$ ]] || [ "$PY_MEMBERS" -lt 1 ]; then
  echo "[sim] ERROR: --py-members must be a positive integer, got '$PY_MEMBERS'" >&2
  exit 1
fi

case "$SUITE" in
  all|core|failover|python)
    ;;
  *)
    echo "[sim] ERROR: --suite must be one of all|core|failover|python, got '$SUITE'" >&2
    exit 1
    ;;
esac

PYTHON_LOG="$LOG_DIR/python_1v${PY_MEMBERS}.log"

mkdir -p "$LOG_DIR" "$BUILD_DIR"

if [ "$SUITE" != "python" ]; then
  CC_BIN="${CC:-cc}"
  if ! command -v "$CC_BIN" >/dev/null 2>&1; then
    if command -v clang >/dev/null 2>&1; then
      CC_BIN="clang"
    elif command -v gcc >/dev/null 2>&1; then
      CC_BIN="gcc"
    else
      echo "[sim] ERROR: no C compiler found (cc/clang/gcc)." >&2
      exit 1
    fi
  fi
else
  CC_BIN="(skip)"
fi

if [ "$SUITE" != "core" ]; then
  if ! command -v "$PYTHON_BIN" >/dev/null 2>&1; then
    echo "[sim] ERROR: python runtime not found: $PYTHON_BIN" >&2
    exit 1
  fi
  if [ ! -f "$PYTHON_SIM_SCRIPT" ]; then
    echo "[sim] ERROR: python simulation script not found: $PYTHON_SIM_SCRIPT" >&2
    exit 1
  fi
fi

CFLAGS="-Wall -Werror -I$ROOT_DIR/include"

echo "[sim] using compiler: $CC_BIN"
echo "[sim] python runtime: $PYTHON_BIN"
echo "[sim] build dir: $BUILD_DIR"
echo "[sim] stress iterations: $ITERATIONS"
echo "[sim] suite: $SUITE"

if [ "$SUITE" = "core" ] || [ "$SUITE" = "all" ]; then
  echo "[sim] building network simulation..."
  "$CC_BIN" $CFLAGS \
    "$ROOT_DIR/examples/team_network_demo.c" \
    "$ROOT_DIR/src/sle_team_packet.c" \
    "$ROOT_DIR/src/sle_team_node.c" \
    "$ROOT_DIR/src/sle_team_location.c" \
    "$ROOT_DIR/src/sle_team_relay_optimizer.c" \
    "$ROOT_DIR/src/sle_team_web_api.c" \
    -DSLE_TEAM_NETWORK_TEST \
    -o "$NETWORK_BIN"

  echo "[sim] building packet simulation..."
  "$CC_BIN" $CFLAGS \
    "$ROOT_DIR/examples/team_node_common.c" \
    "$ROOT_DIR/src/sle_team_packet.c" \
    "$ROOT_DIR/src/sle_team_node.c" \
    "$ROOT_DIR/src/sle_team_location.c" \
    -DSLE_TEAM_PACKET_TEST \
    -o "$PACKET_BIN"

  echo "[sim] building node regression simulation..."
  "$CC_BIN" $CFLAGS \
    "$ROOT_DIR/examples/team_node_regression_test.c" \
    "$ROOT_DIR/src/sle_team_packet.c" \
    "$ROOT_DIR/src/sle_team_nmea.c" \
    "$ROOT_DIR/src/sle_team_node.c" \
    "$ROOT_DIR/src/sle_team_location.c" \
    "$ROOT_DIR/src/sle_team_relay_optimizer.c" \
    -DSLE_TEAM_NODE_REGRESSION_TEST \
    -o "$REGRESSION_BIN"
fi

if [ "$SUITE" = "failover" ] || [ "$SUITE" = "all" ]; then
  echo "[sim] building relay rebalance simulation..."
  "$CC_BIN" $CFLAGS \
    "$ROOT_DIR/examples/relay_rebalance_demo.c" \
    -o "$REBALANCE_BIN"
  echo "[sim] building failover suite simulation..."
  "$CC_BIN" $CFLAGS \
    "$ROOT_DIR/examples/relay_failover_suite.c" \
    -o "$FAILOVER_BIN"
fi

PASS_COUNT=0
FAIL_COUNT=0
FAIL_ITERS=""
LAST_ERR=""

: > "$NETWORK_LOG"
: > "$PACKET_LOG"
: > "$REGRESSION_LOG"
: > "$REBALANCE_LOG"
: > "$FAILOVER_LOG"
: > "$PYTHON_LOG"

run_python_once() {
  local i="$1"
  local pylog="$2"
  local run_seed
  local py_cmd

  run_seed=$((PY_SEED + i))
  py_cmd=(
    "$PYTHON_BIN" "$PYTHON_SIM_SCRIPT"
    --members "$PY_MEMBERS"
    --direct-cap "$PY_DIRECT_CAP"
    --relay-fail-tick "$PY_FAIL_TICK"
    --relay-recover-tick "$PY_RECOVER_TICK"
    --ticks "$PY_TICKS"
    --relay-target "$PY_RELAY_TARGET"
    --seed "$run_seed"
    --stress 1
    --packet-loss-rate "$PY_PACKET_LOSS_RATE"
    --jitter-min-ms "$PY_JITTER_MIN_MS"
    --jitter-max-ms "$PY_JITTER_MAX_MS"
    --batch-fail-relay-count "$PY_BATCH_FAIL_RELAY_COUNT"
  )
  if [ -n "$PY_BATCH_FAIL_RELAY_TICKS" ]; then
    py_cmd+=(--batch-fail-relay-ticks "$PY_BATCH_FAIL_RELAY_TICKS")
  fi
  if [ "$PY_SHOW_TIMELINE" -ne 0 ]; then
    py_cmd+=(--show-timeline)
  fi
  "${py_cmd[@]}" > "$pylog" 2>&1
}

run_once() {
  local i="$1"
  local nlog="$LOG_DIR/network_test.iter${i}.log"
  local plog="$LOG_DIR/packet_test.iter${i}.log"
  local glog="$LOG_DIR/team_node_regression_test.iter${i}.log"
  local rlog="$LOG_DIR/relay_rebalance_test.iter${i}.log"
  local flog="$LOG_DIR/failover_suite.iter${i}.log"
  local pylog="$LOG_DIR/python_1v${PY_MEMBERS}.iter${i}.log"
  local iter_err=""

  if [ "$SUITE" = "core" ]; then
    if ! "$NETWORK_BIN" > "$nlog" 2>&1; then
      iter_err="$nlog"
    elif ! "$PACKET_BIN" > "$plog" 2>&1; then
      iter_err="$plog"
    elif ! "$REGRESSION_BIN" > "$glog" 2>&1; then
      iter_err="$glog"
    else
      PASS_COUNT=$((PASS_COUNT + 1))
      cat "$nlog" >> "$NETWORK_LOG"
      cat "$plog" >> "$PACKET_LOG"
      cat "$glog" >> "$REGRESSION_LOG"
      return 0
    fi
  elif [ "$SUITE" = "python" ]; then
    if ! run_python_once "$i" "$pylog"; then
      iter_err="$pylog"
    else
      PASS_COUNT=$((PASS_COUNT + 1))
      cat "$pylog" >> "$PYTHON_LOG"
      return 0
    fi
  elif [ "$SUITE" = "failover" ]; then
    if ! "$REBALANCE_BIN" > "$rlog" 2>&1; then
      iter_err="$rlog"
    elif ! "$FAILOVER_BIN" > "$flog" 2>&1; then
      iter_err="$flog"
    elif ! run_python_once "$i" "$pylog"; then
      iter_err="$pylog"
    else
      PASS_COUNT=$((PASS_COUNT + 1))
      cat "$rlog" >> "$REBALANCE_LOG"
      cat "$flog" >> "$FAILOVER_LOG"
      cat "$pylog" >> "$PYTHON_LOG"
      return 0
    fi
  else
    if ! "$NETWORK_BIN" > "$nlog" 2>&1; then
      iter_err="$nlog"
    elif ! "$PACKET_BIN" > "$plog" 2>&1; then
      iter_err="$plog"
    elif ! "$REGRESSION_BIN" > "$glog" 2>&1; then
      iter_err="$glog"
    elif ! "$REBALANCE_BIN" > "$rlog" 2>&1; then
      iter_err="$rlog"
    elif ! "$FAILOVER_BIN" > "$flog" 2>&1; then
      iter_err="$flog"
    elif ! run_python_once "$i" "$pylog"; then
      iter_err="$pylog"
    else
      PASS_COUNT=$((PASS_COUNT + 1))
      cat "$nlog" >> "$NETWORK_LOG"
      cat "$plog" >> "$PACKET_LOG"
      cat "$glog" >> "$REGRESSION_LOG"
      cat "$rlog" >> "$REBALANCE_LOG"
      cat "$flog" >> "$FAILOVER_LOG"
      cat "$pylog" >> "$PYTHON_LOG"
      return 0
    fi
  fi

  FAIL_COUNT=$((FAIL_COUNT + 1))
  FAIL_ITERS+=" ${i}"
  if [ -n "$iter_err" ] && [ -z "$LAST_ERR" ]; then
    LAST_ERR="$iter_err"
  fi
  {
    echo "[sim] iteration $i failed"
    if [ "$SUITE" = "core" ] || [ "$SUITE" = "all" ]; then
      echo "---- network ----"
      cat "$nlog" || true
      echo "---- packet ----"
      cat "$plog" || true
      echo "---- node regression ----"
      cat "$glog" || true
    fi
    if [ "$SUITE" = "failover" ] || [ "$SUITE" = "all" ]; then
      echo "---- relay rebalance ----"
      cat "$rlog" || true
      echo "---- failover suite ----"
      cat "$flog" || true
    fi
    if [ "$SUITE" != "core" ]; then
      echo "---- python 1v20 ----"
      cat "$pylog" || true
    fi
  } >> "$NETWORK_LOG"
  return 1
}

echo "[sim] running simulations..."
for ((i=1; i<=ITERATIONS; i++)); do
  printf '[sim] iteration %d/%d\n' "$i" "$ITERATIONS"
  run_once "$i" || true
done

echo "[sim] summary: pass=$PASS_COUNT fail=$FAIL_COUNT total=$ITERATIONS"
if [ "$FAIL_COUNT" -ne 0 ]; then
  echo "[sim] failed iterations:$FAIL_ITERS"
  echo "[sim] first failure log: $LAST_ERR"
  exit 1
fi

echo "[sim] done"
if [ "$SUITE" = "core" ] || [ "$SUITE" = "all" ]; then
  echo "[sim] network log: $NETWORK_LOG"
  echo "[sim] packet  log: $PACKET_LOG"
  echo "[sim] node    log: $REGRESSION_LOG"
fi
if [ "$SUITE" = "failover" ] || [ "$SUITE" = "all" ]; then
  echo "[sim] relay   log: $REBALANCE_LOG"
  echo "[sim] failover log: $FAILOVER_LOG"
fi
if [ "$SUITE" != "core" ]; then
  echo "[sim] python log: $PYTHON_LOG"
fi
