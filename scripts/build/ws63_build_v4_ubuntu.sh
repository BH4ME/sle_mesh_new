#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'USAGE'
Usage:
  scripts/build/ws63_build_v4_ubuntu.sh unified

Builds the v4 WS63 team firmware on a LAN Ubuntu build machine.

Environment is the same as scripts/build/ws63_build_team_ubuntu.sh:
  UBUNTU_HOST=192.168.1.50
  UBUNTU_PORT=22
  UBUNTU_USER=builder
  UBUNTU_PASS=builder
  UBUNTU_SDK=/home/builder/workspace/bearpi-pico_h3863_fresh
  OUT_ROOT=/path/to/output_from_vm
  BUILD_JOBS=4
USAGE
}

role="${1:-unified}"
case "$role" in
  unified|leader|member)
    self_id=1
    out_dir="team_network_v4_unified_runtime_role"
    out_name="ws63-liteos-app_v4_unified_all.fwpkg"
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

next_archive_path() {
  local latest="$1"
  local version="$2"
  local base="${latest%.fwpkg}_${version}"
  local candidate="${base}.fwpkg"
  local index=1
  while [[ -e "$candidate" ]]; do
    candidate="${base}.${index}.fwpkg"
    index=$((index + 1))
  done
  printf '%s\n' "$candidate"
}

if [[ -z "$UBUNTU_HOST" ]]; then
  echo "UBUNTU_HOST is required, for example: UBUNTU_HOST=192.168.1.50 $0 $role" >&2
  exit 2
fi

CONFIG_PATH="$UBUNTU_SDK/build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
REMOTE_PKG="$UBUNTU_SDK/output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg"
REMOTE_PROTO="$UBUNTU_SDK/third_party/sle_mesh"
REMOTE_APP="$UBUNTU_SDK/application/samples/products/sle_team_network"
LOCAL_OUT="$OUT_ROOT/$out_dir/$out_name"
ARCHIVE_OUT="$(next_archive_path "$LOCAL_OUT" "v4.5.46-minimal")"
LVGL_PATCH="$REMOTE_APP/third_party/lvgl-patches/lv8.3.11-ws63-c89-rect.patch"

ssh_opts=(
  -o StrictHostKeyChecking=no
  -o UserKnownHostsFile=/dev/null
  -o LogLevel=ERROR
)

if [[ -n "$UBUNTU_PASS" ]]; then
  ssh_base=(sshpass -p "$UBUNTU_PASS" ssh "${ssh_opts[@]}")
  rsync_ssh=(sshpass -p "$UBUNTU_PASS" ssh "${ssh_opts[@]}" -p "$UBUNTU_PORT")
else
  ssh_base=(ssh "${ssh_opts[@]}")
  rsync_ssh=(ssh "${ssh_opts[@]}" -p "$UBUNTU_PORT")
fi

ssh_cmd=("${ssh_base[@]}" -p "$UBUNTU_PORT" "$UBUNTU_USER@$UBUNTU_HOST")

echo "WS63 Ubuntu build"
echo "profile:    v4.5.46-minimal unified runtime role (minimal leader/member/relay networking)"
echo "fallback id:$self_id"
echo "host:       $UBUNTU_USER@$UBUNTU_HOST:$UBUNTU_PORT"
echo "sdk:        $UBUNTU_SDK"
echo "archive:    $ARCHIVE_OUT"
echo "latest:     $LOCAL_OUT"
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

"${ssh_cmd[@]}" "bash -s -- '$LVGL_PATCH' '$REMOTE_APP'" <<'SH'
LVGL_PATCH="$1"
REMOTE_APP="$2"
if [ -f "$LVGL_PATCH" ]; then
  cd "$REMOTE_APP/third_party/lvgl"
  if grep -q "lv_area_t center_coords;" src/draw/sw/lv_draw_sw_rect.c &&
    grep -q "bool mask_any_center = false;" src/draw/sw/lv_draw_sw_rect.c; then
    echo "LVGL patch already present in source"
  elif git apply --unidiff-zero --check "$LVGL_PATCH"; then
    git apply --unidiff-zero "$LVGL_PATCH"
  elif git apply --unidiff-zero --reverse --check "$LVGL_PATCH"; then
    echo "LVGL patch already applied"
  else
    echo "LVGL patch check failed: $LVGL_PATCH" >&2
    exit 1
  fi
fi
SH

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

def unset_kconfig_bool(text, key):
    lines = text.splitlines()
    found = False
    out = []
    for line in lines:
        if line.startswith(key + "=") or line.startswith(f"# {key} is not set"):
            if not found:
                out.append(f"# {key} is not set")
                found = True
            continue
        out.append(line)
    if not found:
        out.append(f"# {key} is not set")
    return "\n".join(out) + "\n"

s = set_kconfig_value(s, "CONFIG_SLE_TEAM_SELF_ID", self_id)
s = set_kconfig_value(s, "CONFIG_SAMPLE_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SAMPLE_SUPPORT_SLE_TEAM_NETWORK", "y")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART_1_VS_8")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER_1_VS_8")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT_1_VS_8")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_BLE_UART")
s = unset_kconfig_bool(s, "CONFIG_SAMPLE_SUPPORT_SLE_GETAWAY")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_LEADER_ID", "1")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_UART_BUS", "0")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_UART_TXD_PIN", "21")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_UART_RXD_PIN", "22")
s = set_kconfig_value(s, "CONFIG_AT_UART", "3")
s = unset_kconfig_bool(s, "CONFIG_DYNAMIC_UART_ID_BINDDING")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_LED_PIN", "255")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WS2812_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WS2812_PIN", "0")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_BUZZER_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_BUZZER_PIN", "14")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_BUZZER_ACTIVE_HIGH", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_BUZZER_MUTED", "y")
s = unset_kconfig_bool(s, "CONFIG_SLE_TEAM_BUZZER_AUTO_TOGGLE")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_UART_BUS", "1")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_UART_TXD_PIN", "17")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_GPS_UART_RXD_PIN", "18")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_CTRL_PIN", "5")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_VBAT_PIN", "12")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_VBAT_CHANNEL", "5")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS", "50")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S", "30")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_CHRG_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_CHRG_PIN", "2")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_CHRG_ACTIVE_LOW", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_CHRG_EXTERNAL_PULLUP", "y")
s = set_kconfig_value(s, "CONFIG_ADC_SUPPORT_AUTO_SCAN", "y")
s = set_kconfig_value(s, "CONFIG_ADC_USING_V154", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_SPI_BUS", "0")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_SCLK_PIN", "7")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_MOSI_PIN", "9")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_CS_PIN", "8")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_CS_ALWAYS_LOW", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_DC_PIN", "13")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_RESET_PIN", "10")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_X_OFFSET", "40")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_Y_OFFSET", "53")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_WIDTH", "240")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_HEIGHT", "135")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_DISPLAY_USE_LVGL", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_LVGL_DRAW_BUF_LINES", "8")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_HEARTBEAT_INTERVAL_S", "1")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_HEARTBEAT_TIMEOUT_S", "3")
s = set_kconfig_value(s, "CONFIG_SPI_SUPPORT_MASTER", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WIFI_AP_ENABLE", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WIFI_AP_AUTO_START", "y")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WIFI_AP_SSID", '"SLE-TEAM-V4"')
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_PERIPHERAL", "y")
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_CENTRAL", "y")
path.write_text(s)
print("configured v4.5.46-minimal WS63 minimal leader/member/relay networking")
PY

"${ssh_cmd[@]}" "cd '$UBUNTU_SDK' && python3 build.py -c ws63-liteos-app -j'$BUILD_JOBS'"

"${ssh_cmd[@]}" "python3 - '$UBUNTU_SDK'" <<'PY'
from pathlib import Path
import sys

sdk = Path(sys.argv[1])
cfg_path = sdk / "build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
map_path = sdk / "output/ws63/acore/ws63-liteos-app/ws63-liteos-app.map"
elf_path = sdk / "output/ws63/acore/ws63-liteos-app/ws63-liteos-app.elf"
app_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_network_app.c"
http_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_http.c"
sle_client_source_path = sdk / "application/samples/products/sle_team_network/sle_uart_client/sle_uart_client.c"
sle_server_source_path = sdk / "application/samples/products/sle_team_network/sle_uart_server/sle_uart_server.c"
ws2812_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_ws2812.c"
gps_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_gps.c"
status_led_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_status_led.c"
st7789_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_st7789_display.c"
nmea_source_path = sdk / "third_party/sle_mesh/src/sle_team_nmea.c"
proto_source_path = sdk / "third_party/sle_mesh/src/sle_team_node.c"

cfg = cfg_path.read_text(errors="replace")
map_text = map_path.read_text(errors="replace")
elf = elf_path.read_bytes()
app_source = app_source_path.read_text(errors="replace")
http_source = http_source_path.read_text(errors="replace")
sle_client_source = sle_client_source_path.read_text(errors="replace")
sle_server_source = sle_server_source_path.read_text(errors="replace")
ws2812_source = ws2812_source_path.read_text(errors="replace")
gps_source = gps_source_path.read_text(errors="replace")
status_led_source = status_led_source_path.read_text(errors="replace")
st7789_source = st7789_source_path.read_text(errors="replace")
nmea_source = nmea_source_path.read_text(errors="replace")
proto_source = proto_source_path.read_text(errors="replace")

VERSION = "v4.5.46-minimal"

def require_text(name, source, needle):
    if needle not in source:
        raise SystemExit(f"post-build guard failed: {name} missing {needle}")

def reject_text(name, source, needle):
    if needle in source:
        raise SystemExit(f"post-build guard failed: {name} still contains retired token {needle}")

for item in [
    "CONFIG_SAMPLE_SUPPORT_SLE_TEAM_NETWORK=y",
    "CONFIG_SUPPORT_SLE_PERIPHERAL=y",
    "CONFIG_SUPPORT_SLE_CENTRAL=y",
    "CONFIG_SLE_TEAM_WS2812_ENABLE=y",
    "CONFIG_SLE_TEAM_WS2812_PIN=0",
    "CONFIG_SLE_TEAM_GPS_ENABLE=y",
    "CONFIG_SLE_TEAM_GPS_UART_BUS=1",
    "CONFIG_SLE_TEAM_GPS_UART_TXD_PIN=17",
    "CONFIG_SLE_TEAM_GPS_UART_RXD_PIN=18",
]:
    require_text("kconfig", cfg, item)

for forbidden in [
    "CONFIG_SAMPLE_SUPPORT_SLE_UART=y",
    "CONFIG_SAMPLE_SUPPORT_SLE_UART_1_VS_8=y",
    "CONFIG_SAMPLE_SUPPORT_SLE_UART_CLIENT_1_VS_8=y",
    "CONFIG_SAMPLE_SUPPORT_SLE_UART_SERVER_1_VS_8=y",
]:
    reject_text("kconfig", cfg, forbidden)

for item in [
    "ws63_team_network_app.c.obj",
    "ws63_team_http.c.obj",
    "ws63_team_gps.c.obj",
    "ws63_st7789_display.c.obj",
    "ws63_team_status_led.c.obj",
    "ws63_ws2812.c.obj",
    "sle_team_nmea.c.obj",
    "sle_team_node.c.obj",
    "sle_team_location.c.obj",
    "sle_team_relay_optimizer.c.obj",
]:
    require_text("linked map", map_text, item)
reject_text("linked map", map_text, "sle_team_leader_migration.c.obj")

for item in [
    VERSION.encode("ascii"),
    b"minimal leader/member/relay rewrite",
    b"[cfg-json]",
    b"cfg direct",
    b"cfg relay target",
    b"pairing start",
    b"members",
    b"state",
    b"direct_cap",
]:
    if item not in elf:
        raise SystemExit(f"post-build guard failed: ELF missing {item.decode('ascii', errors='replace')}")

for name, source, item in [
    ("ws63_team_network_app.c", app_source, f'#define SLE_TEAM_FW_VERSION "{VERSION}"'),
    ("ws63_team_network_app.c", app_source, "minimal leader/member/relay rewrite"),
    ("ws63_team_network_app.c", app_source, "#define SLE_TEAM_DIRECT_CAP_DEFAULT 7U"),
    ("ws63_team_network_app.c", app_source, "team_physical_connect_limit"),
    ("ws63_team_network_app.c", app_source, "sle_uart_client_set_connect_limit(team_physical_connect_limit())"),
    ("ws63_team_network_app.c", app_source, "sle_uart_client_init(team_client_rx_cb, team_client_rx_cb)"),
    ("ws63_team_network_app.c", app_source, "sle_uart_server_init(team_server_read_cb, team_server_write_cb)"),
    ("ws63_team_network_app.c", app_source, "sle_uart_client_send_by_conn"),
    ("ws63_team_network_app.c", app_source, "sle_uart_server_send_report_by_handle"),
    ("ws63_team_network_app.c", app_source, "[cfg-json]"),
    ("ws63_team_network_app.c", app_source, "cfg leader now"),
    ("ws63_team_network_app.c", app_source, "cfg member now"),
    ("ws63_team_network_app.c", app_source, "cfg direct"),
    ("ws63_team_network_app.c", app_source, "cfg relay target"),
    ("ws63_team_network_app.c", app_source, "team_relay_client_start_if_ready"),
    ("ws63_team_network_app.c", app_source, "static sle_team_web_event_log_t g_team_events"),
    ("ws63_team_network_app.c", app_source, "sle_team_web_event_log_init(&g_team_events)"),
    ("ws63_team_network_app.c", app_source, "ws63_team_http_start(&g_team_node, &g_team_events"),
    ("ws63_team_network_app.c", app_source, "ws63_team_status_led_init(); ws63_team_gps_init();"),
    ("ws63_team_network_app.c", app_source, "TeamDisplayTask"),
    ("ws63_team_network_app.c", app_source, "team_display_spawn_task();"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_init(&cfg)"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_show_status"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_show_event"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_tick();"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_init"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_show_status"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_show_event"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_tick"),
    ("ws63_team_http.c", http_source, "/api/location"),
    ("ws63_team_http.c", http_source, "team_http_handle_location"),
    ("ws63_team_http.c", http_source, "sle_team_node_record_local_position"),
    ("ws63_team_http.c", http_source, "sle_team_node_send_position"),
    ("ws63_team_gps.c", gps_source, "team_gps_init"),
    ("ws63_team_gps.c", gps_source, "sle_team_nmea_feed"),
    ("ws63_team_gps.c", gps_source, "sle_team_node_send_position"),
    ("sle_team_nmea.c", nmea_source, "sle_team_nmea_parse_line"),
    ("sle_team_nmea.c", nmea_source, "nmea_commit_rmc"),
    ("sle_team_nmea.c", nmea_source, "nmea_commit_fix_from_gga"),
    ("ws63_team_status_led.c", status_led_source, "g_status_led_breathe_idle"),
    ("ws63_team_status_led.c", status_led_source, "status_led_apply_scaled"),
    ("ws63_ws2812.c", ws2812_source, "ws63_ws2812_set_rgb"),
    ("ws63_ws2812.c", ws2812_source, "ws63_ws2812_encode_grb"),
    ("sle_team_node.c", proto_source, "#define SLE_TEAM_DIRECT_CAP_DEFAULT 7U"),
    ("sle_team_node.c", proto_source, "#define SLE_TEAM_RELAY_CHILD_CAP_DEFAULT 7U"),
    ("sle_team_node.c", proto_source, "#define SLE_TEAM_MEMBER_HELLO_INTERVAL_S 1U"),
    ("sle_team_node.c", proto_source, "sle_team_assign_parent"),
    ("sle_team_node.c", proto_source, "sle_team_select_online_relay"),
    ("sle_team_node.c", proto_source, "sle_team_node_grant_relay"),
    ("sle_team_node.c", proto_source, "policy_pending"),
    ("sle_team_node.c", proto_source, "pending_ack_seq"),
    ("sle_team_node.c", proto_source, "sle_team_node_member_link_lost"),
    ("sle_team_node.c", proto_source, "sle_team_forward_packet"),
    ("sle_team_node.c", proto_source, "SLE_TEAM_NET_WAIT_POLICY"),
    ("sle_team_node.c", proto_source, "SLE_TEAM_PARENT_WAIT_POLICY"),
    ("sle_team_relay_optimizer.c", (sdk / "third_party/sle_mesh/src/sle_team_relay_optimizer.c").read_text(errors="replace"), "sle_team_relay_optimizer_run"),
    ("sle_uart_client.c", sle_client_source, "sle_uart_client_send_by_conn"),
    ("sle_uart_server.c", sle_server_source, "sle_uart_server_send_report_by_conn"),
]:
    require_text(name, source, item)

retired_tokens = [
    "TEAM_LEADER_NET_STAGE",
    "team_leader_topology_",
    "topology_frozen",
    "topology_policy",
    "stable_rejoin",
    "route_hint",
    "failover",
    "leader_logical_upstream",
    "try_parent_switch",
    "reselect",
    "DISCOVERING",
    "FREEZING",
    "PROVISIONING",
    "STABLE",
    "routeMetrics",
    "runtimeRelayBudget",
    "runtimeNetworkStage",
    "LeaderNetworkStage",
    "topology frozen",
    "topology policy",
]
for token in retired_tokens:
    reject_text("active app source", app_source, token)
    reject_text("active node source", proto_source, token)

network_task_body = app_source.split('static void *team_network_task(const char *arg)', 1)[1].split(
    'static int team_display_init_panel(void)', 1
)[0]
reject_text("TeamNetworkTask body", network_task_body, "ws63_st7789_tick();")

app_lines = len(app_source.splitlines())
node_lines = len(proto_source.splitlines())
if app_lines > 2200 or node_lines > 2200:
    raise SystemExit(f"post-build guard failed: minimal sources grew too large app={app_lines} node={node_lines}")

print(f"post-build guard passed: {VERSION} minimal networking app_lines={app_lines} node_lines={node_lines}")
sys.exit(0)


PY

mkdir -p "$(dirname "$LOCAL_OUT")"
rsync -az -e "${rsync_ssh[*]}" "$UBUNTU_USER@$UBUNTU_HOST:$REMOTE_PKG" "$ARCHIVE_OUT"
cp "$ARCHIVE_OUT" "$LOCAL_OUT"
ls -lh "$ARCHIVE_OUT" "$LOCAL_OUT"
