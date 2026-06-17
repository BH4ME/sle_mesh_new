#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/build/ws63_build_team_ubuntu.sh unified

Builds the WS63 team firmware on a LAN Ubuntu build machine.

The script always syncs the local protocol and board app sources into the
BearPi SDK before building, then copies the resulting .fwpkg back to the Mac.

Environment:
  UBUNTU_HOST=192.168.1.50
  UBUNTU_PORT=22
  UBUNTU_USER=builder
  UBUNTU_PASS=builder              optional; omit when SSH key login works
  UBUNTU_SDK=/home/builder/workspace/bearpi-pico_h3863_fresh
  OUT_ROOT=/path/to/output_from_vm
  BUILD_JOBS=4

Expected remote SDK layout:
  $UBUNTU_SDK/third_party/sle_mesh/
  $UBUNTU_SDK/application/samples/products/sle_team_network/
USAGE
}

role="${1:-unified}"
case "$role" in
  unified|leader|member)
    self_id=1
    out_dir="team_network_unified_runtime_role"
    out_name="ws63-liteos-app_unified_all.fwpkg"
    ;;
  -h|--help|"")
    usage
    exit 0
    ;;
  *)
    echo "Unknown role: $role" >&2
    usage >&2
    exit 2
    ;;
esac

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
UBUNTU_HOST="${UBUNTU_HOST:-}"
UBUNTU_PORT="${UBUNTU_PORT:-22}"
UBUNTU_USER="${UBUNTU_USER:-builder}"
UBUNTU_PASS="${UBUNTU_PASS:-}"
UBUNTU_SDK="${UBUNTU_SDK:-/home/builder/workspace/bearpi-pico_h3863_fresh}"
OUT_ROOT="${OUT_ROOT:-$ROOT_DIR/output_from_vm}"
BUILD_JOBS="${BUILD_JOBS:-4}"

if [[ -z "$UBUNTU_HOST" ]]; then
  echo "UBUNTU_HOST is required, for example: UBUNTU_HOST=192.168.1.50 $0 $role" >&2
  exit 2
fi

CONFIG_PATH="$UBUNTU_SDK/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
REMOTE_PKG="$UBUNTU_SDK/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg"
REMOTE_PROTO="$UBUNTU_SDK/third_party/sle_mesh"
REMOTE_APP="$UBUNTU_SDK/application/samples/products/sle_team_network"
LOCAL_OUT="$OUT_ROOT/$out_dir/$out_name"

if [[ -n "$UBUNTU_PASS" ]]; then
  ssh_base=(sshpass -p "$UBUNTU_PASS" ssh)
  rsync_ssh=(sshpass -p "$UBUNTU_PASS" ssh -p "$UBUNTU_PORT")
else
  ssh_base=(ssh)
  rsync_ssh=(ssh -p "$UBUNTU_PORT")
fi

ssh_cmd=("${ssh_base[@]}" -p "$UBUNTU_PORT" "$UBUNTU_USER@$UBUNTU_HOST")

echo "WS63 Ubuntu build"
echo "profile:    unified runtime role"
echo "fallback id:$self_id"
echo "host:       $UBUNTU_USER@$UBUNTU_HOST:$UBUNTU_PORT"
echo "sdk:        $UBUNTU_SDK"
echo "local out:  $LOCAL_OUT"
echo

"${ssh_cmd[@]}" "test -f '$CONFIG_PATH' && mkdir -p '$REMOTE_PROTO' '$REMOTE_APP'"

rsync -az --delete \
  --exclude '.git' \
  --exclude 'build' \
  --exclude 'dist' \
  --exclude 'node_modules' \
  -e "${rsync_ssh[*]}" \
  "$ROOT_DIR/include/" "$UBUNTU_USER@$UBUNTU_HOST:$REMOTE_PROTO/include/"

rsync -az --delete \
  --exclude '.git' \
  --exclude 'build' \
  --exclude 'dist' \
  --exclude 'node_modules' \
  -e "${rsync_ssh[*]}" \
  "$ROOT_DIR/src/" "$UBUNTU_USER@$UBUNTU_HOST:$REMOTE_PROTO/src/"

rsync -az --delete \
  --exclude '.git' \
  --exclude 'build' \
  --exclude 'dist' \
  --exclude 'node_modules' \
  -e "${rsync_ssh[*]}" \
  "$ROOT_DIR/xc/ws63_team_network/" "$UBUNTU_USER@$UBUNTU_HOST:$REMOTE_APP/"

"${ssh_cmd[@]}" "python3 - '$CONFIG_PATH' '$self_id'" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
self_id = sys.argv[2]
s = path.read_text()

def set_kconfig_value(text, key, value):
    lines = text.splitlines()
    found = False
    out = []
    for line in lines:
        if line.startswith(key + "=") or line.startswith(f"# {key} is not set"):
            if not found:
                out.append(f"{key}={value}")
                found = True
            continue
        out.append(line)
    if not found:
        out.append(f"{key}={value}")
    return "\n".join(out) + "\n"

s = set_kconfig_value(s, "CONFIG_SLE_TEAM_SELF_ID", self_id)
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_LEADER_ID", "1")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WIFI_AP_SSID", '"SLE-TEAM-WS63"')
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_PERIPHERAL", "y")
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_CENTRAL", "y")
path.write_text(s)
print(f"configured unified runtime role self_fallback={self_id} with central+peripheral enabled")
PY

"${ssh_cmd[@]}" "cd '$UBUNTU_SDK' && python3 build.py ws63-liteos-app -j'$BUILD_JOBS'"

mkdir -p "$(dirname "$LOCAL_OUT")"
rsync -az -e "${rsync_ssh[*]}" "$UBUNTU_USER@$UBUNTU_HOST:$REMOTE_PKG" "$LOCAL_OUT"
ls -lh "$LOCAL_OUT"
