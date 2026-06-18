#!/usr/bin/env python3
"""Build the WS63 v4 unified firmware on the LAN Ubuntu SDK host.

This is the Python/Paramiko equivalent of scripts/build/ws63_build_v4_ubuntu.sh.
It avoids depending on local ssh/rsync/WSL tooling and prints visible progress.
"""

from __future__ import annotations

import argparse
import os
import posixpath
import shutil
import sys
import tarfile
import tempfile
import time
from pathlib import Path

import paramiko


VERSION = "v4.5.64-minimal"
REMOTE_PROTO_REL = "third_party/sle_mesh"
REMOTE_APP_REL = "application/samples/products/sle_team_network"
REMOTE_PKG_REL = "output/ws63/fwpkg/ws63-liteos-app/ws63-liteos-app_all.fwpkg"
CONFIG_REL = "build/config/target_config/ws63/menuconfig/acore/ws63_liteos_app.config"
LOCAL_OUT_REL = "output_from_vm/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg"
EXCLUDES = {".git", "build", "dist", "node_modules", "__pycache__"}


def repo_root() -> Path:
    return Path(__file__).resolve().parents[3]


def log(message: str) -> None:
    print(message, flush=True)


def quote(value: str) -> str:
    return "'" + value.replace("'", "'\"'\"'") + "'"


def add_dir_to_tar(tar: tarfile.TarFile, src: Path, arc_root: str) -> None:
    for path in src.rglob("*"):
        rel = path.relative_to(src)
        if any(part in EXCLUDES for part in rel.parts):
            continue
        arcname = str(Path(arc_root) / rel).replace("\\", "/")
        tar.add(path, arcname=arcname, recursive=False)


def make_archive(src: Path, arc_root: str, dst: Path) -> None:
    with tarfile.open(dst, "w:gz") as tar:
        add_dir_to_tar(tar, src, arc_root)


def run_remote(client: paramiko.SSHClient, command: str, stage: str, timeout: int | None = None) -> str:
    log(f"[remote] {stage}")
    stdin, stdout, stderr = client.exec_command(command, get_pty=True, timeout=timeout)
    stdin.close()
    output_parts: list[str] = []
    while not stdout.channel.exit_status_ready():
        while stdout.channel.recv_ready():
            chunk = stdout.channel.recv(4096).decode("utf-8", errors="replace")
            output_parts.append(chunk)
            print(chunk, end="", flush=True)
        time.sleep(0.1)
    while stdout.channel.recv_ready():
        chunk = stdout.channel.recv(4096).decode("utf-8", errors="replace")
        output_parts.append(chunk)
        print(chunk, end="", flush=True)
    while stderr.channel.recv_stderr_ready():
        chunk = stderr.channel.recv_stderr(4096).decode("utf-8", errors="replace")
        output_parts.append(chunk)
        print(chunk, end="", flush=True)
    status = stdout.channel.recv_exit_status()
    if status != 0:
        raise RuntimeError(f"remote stage failed ({stage}) exit={status}")
    return "".join(output_parts)


def upload_file(sftp: paramiko.SFTPClient, local: Path, remote: str) -> None:
    log(f"[upload] {local.name} -> {remote}")
    sftp.put(str(local), remote)


def download_file(sftp: paramiko.SFTPClient, remote: str, local: Path) -> None:
    local.parent.mkdir(parents=True, exist_ok=True)
    log(f"[download] {remote} -> {local}")
    sftp.get(remote, str(local))


def versioned_output_path(latest_output: Path, version: str) -> Path:
    return latest_output.with_name(f"{latest_output.stem}_{version}{latest_output.suffix}")


def reserve_unique_path(path: Path) -> Path:
    if not path.exists():
        return path
    for index in range(1, 1000):
        candidate = path.with_name(f"{path.stem}.{index}{path.suffix}")
        if not candidate.exists():
            return candidate
    raise RuntimeError(f"could not reserve unique firmware output path near {path}")


def build_config_script() -> str:
    return r'''
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
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_CS_ALWAYS_LOW", "n")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_DC_PIN", "10")
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_ST7789_RESET_PIN", "13")
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
s = set_kconfig_value(s, "CONFIG_SLE_TEAM_WIFI_AP_SSID", '"SLE"')
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_PERIPHERAL", "y")
s = set_kconfig_value(s, "CONFIG_SUPPORT_SLE_CENTRAL", "y")
path.write_text(s)
print("configured v4.5.64-minimal WS63 minimal leader/member/relay networking")
'''


def post_build_guard_script() -> str:
    return r'''
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
power_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_power.c"
status_led_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_team_status_led.c"
st7789_source_path = sdk / "application/samples/products/sle_team_network/src/ws63_st7789_display.c"
nmea_source_path = sdk / "third_party/sle_mesh/src/sle_team_nmea.c"
proto_source_path = sdk / "third_party/sle_mesh/src/sle_team_node.c"
web_api_source_path = sdk / "third_party/sle_mesh/src/sle_team_web_api.c"

cfg = cfg_path.read_text(errors="replace")
map_text = map_path.read_text(errors="replace")
elf = elf_path.read_bytes()
app_source = app_source_path.read_text(errors="replace")
http_source = http_source_path.read_text(errors="replace")
sle_client_source = sle_client_source_path.read_text(errors="replace")
sle_server_source = sle_server_source_path.read_text(errors="replace")
ws2812_source = ws2812_source_path.read_text(errors="replace")
gps_source = gps_source_path.read_text(errors="replace")
power_source = power_source_path.read_text(errors="replace")
status_led_source = status_led_source_path.read_text(errors="replace")
st7789_source = st7789_source_path.read_text(errors="replace")
nmea_source = nmea_source_path.read_text(errors="replace")
proto_source = proto_source_path.read_text(errors="replace")
web_api_source = web_api_source_path.read_text(errors="replace")

VERSION = "v4.5.64-minimal"

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
    "CONFIG_SLE_TEAM_ADC_ENABLE=y",
    "CONFIG_SLE_TEAM_ADC_CTRL_PIN=5",
    "CONFIG_SLE_TEAM_ADC_VBAT_PIN=12",
    "CONFIG_SLE_TEAM_ADC_VBAT_CHANNEL=5",
    "CONFIG_SLE_TEAM_ADC_CTRL_ACTIVE_HIGH=y",
    "CONFIG_SLE_TEAM_ADC_SAMPLE_SETTLE_MS=50",
    "CONFIG_SLE_TEAM_ADC_SAMPLE_INTERVAL_S=30",
    "CONFIG_SLE_TEAM_CHRG_ENABLE=y",
    "CONFIG_SLE_TEAM_CHRG_PIN=2",
    "CONFIG_SLE_TEAM_CHRG_ACTIVE_LOW=y",
    "CONFIG_SLE_TEAM_CHRG_EXTERNAL_PULLUP=y",
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
    "ws63_team_power.c.obj",
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
    ("ws63_team_network_app.c", app_source, "static const ws63_team_http_callbacks_t g_team_http_callbacks"),
    ("ws63_team_network_app.c", app_source, "team_http_get_identity_cb"),
    ("ws63_team_network_app.c", app_source, "team_http_member_leave_cb"),
    ("ws63_team_network_app.c", app_source, "ws63_team_http_start(&g_team_node, &g_team_events, &g_team_http_callbacks"),
    ("ws63_team_network_app.c", app_source, "role_request_pending"),
    ("ws63_team_network_app.c", app_source, "team_request_role_config"),
    ("ws63_team_network_app.c", app_source, "team_handle_role_request_once"),
    ("ws63_team_network_app.c", app_source, "roleRequestPending"),
    ("ws63_team_http.c", http_source, "roleRequestPending"),
    ("ws63_team_http.c", http_source, "starting SLE"),
    ("ws63_team_network_app.c", app_source, '"SLE-%02X%02X"'),
    ("ws63_team_network_app.c", app_source, "ws63_team_status_led_init();"),
    ("ws63_team_network_app.c", app_source, "ws63_team_gps_init();"),
    ("ws63_team_network_app.c", app_source, "ws63_team_power_init();"),
    ("ws63_team_network_app.c", app_source, "ws63_team_power_tick(0U);"),
    ("ws63_team_network_app.c", app_source, "ws63_team_power_battery_percent()"),
    ("ws63_team_network_app.c", app_source, "ws63_team_power_cli_handle(msg.line)"),
    ("ws63_team_network_app.c", app_source, "ws63_team_gps_tick(&g_team_node, now_ms, team_battery_percent(NULL));"),
    ("ws63_team_network_app.c", app_source, "TeamDisplayTask"),
    ("ws63_team_network_app.c", app_source, "team_display_spawn_task();"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_init(&cfg)"),
    ("ws63_team_network_app.c", app_source, "#define CONFIG_SLE_TEAM_ST7789_DC_PIN 10"),
    ("ws63_team_network_app.c", app_source, "#define CONFIG_SLE_TEAM_ST7789_RESET_PIN 13"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_show_status"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_show_event"),
    ("ws63_team_network_app.c", app_source, "ws63_st7789_tick();"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_init"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_show_status"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_show_event"),
    ("ws63_st7789_display.c", st7789_source, "ws63_st7789_tick"),
    ("ws63_st7789_display.c", st7789_source, "#define CONFIG_SLE_TEAM_ST7789_CS_ALWAYS_LOW 0"),
    ("ws63_st7789_display.c", st7789_source, "#define ST7789_SOFT_SPI_ENABLE 0"),
    ("ws63_st7789_display.c", st7789_source, "hardware spi enabled"),
    ("ws63_st7789_display.c", st7789_source, "LVGL gives host-endian RGB565; ST7789 SPI wants high byte first"),
    ("ws63_team_http.c", http_source, "/api/location"),
    ("ws63_team_http.c", http_source, "team_http_handle_location"),
    ("ws63_team_http.c", http_source, "sle_team_node_record_local_position"),
    ("ws63_team_http.c", http_source, "sle_team_node_send_position"),
    ("ws63_team_http.c", http_source, "<span class=\\\"v\\\">%s fix=%u sat=%u lat=%ld lon=%ld speed=%u heading=%u</span>"),
    ("ws63_team_http.c", http_source, "no fix fix=%u sat=%u"),
    ("ws63_team_http.c", http_source, "adv.hidden_ssid_flag = 1U"),
    ("ws63_team_http.c", http_source, "TEAM_HTTP_WIFI_SECURITY_PRIMARY"),
    ("ws63_team_http.c", http_source, "TEAM_HTTP_WIFI_PROTOCOL_PRIMARY"),
    ("ws63_team_http.c", http_source, "TEAM_HTTP_WIFI_SECURITY_COMPAT_MIX"),
    ("ws63_team_http.c", http_source, "TEAM_HTTP_WIFI_PROTOCOL_COMPAT_AX"),
    ("ws63_team_http.c", http_source, "conf.security_type = TEAM_HTTP_WIFI_SECURITY_PRIMARY"),
    ("ws63_team_http.c", http_source, "adv.protocol_mode = TEAM_HTTP_WIFI_PROTOCOL_PRIMARY"),
    ("ws63_team_http.c", http_source, "softap enable failed ret=0x%x with wpa2+11bgn, fallback to mix/ax"),
    ("ws63_team_http.c", http_source, "fallback.protocol_mode = TEAM_HTTP_WIFI_PROTOCOL_COMPAT_AX"),
    ("ws63_team_http.c", http_source, "team_http_append_topology"),
    ("ws63_team_http.c", http_source, "onlineNodeCount"),
    ("ws63_team_http.c", http_source, "relayNodeCount"),
    ("sle_team_web_api.c", web_api_source, "onlineNodeCount"),
    ("sle_team_web_api.c", web_api_source, "relayNodeCount"),
    ("ws63_team_network_app.c", app_source, "team_web_event(SLE_TEAM_WEB_EVENT_SYSTEM"),
    ("ws63_team_network_app.c", app_source, "team_web_event(SLE_TEAM_WEB_EVENT_RX"),
    ("ws63_team_gps.c", gps_source, "team_gps_init"),
    ("ws63_team_gps.c", gps_source, "sle_team_nmea_feed"),
    ("ws63_team_gps.c", gps_source, "sle_team_pos_body_t pos = {0};"),
    ("ws63_team_gps.c", gps_source, "sle_team_node_send_position"),
    ("ws63_team_power.c", power_source, "uapi_adc_init(ADC_CLOCK_500KHZ)"),
    ("ws63_team_power.c", power_source, "adc_port_read(g_power.adc_vbat_channel, &adc_mv)"),
    ("ws63_team_power.c", power_source, "adc_ctrl_set(1U);"),
    ("ws63_team_power.c", power_source, "adc_ctrl_set(0U);"),
    ("ws63_team_power.c", power_source, "SLE_TEAM_ADC_DIVIDER_TOP_KOHM"),
    ("ws63_team_power.c", power_source, "SLE_TEAM_ADC_DIVIDER_BOTTOM_KOHM"),
    ("ws63_team_power.c", power_source, "CONFIG_SLE_TEAM_CHRG_PIN"),
    ("ws63_team_power.c", power_source, "ws63_team_power_battery_percent"),
    ("sle_team_nmea.c", nmea_source, "sle_team_nmea_parse_line"),
    ("sle_team_nmea.c", nmea_source, "nmea_commit_rmc"),
    ("sle_team_nmea.c", nmea_source, "nmea_commit_fix_from_gga"),
    ("sle_team_nmea.c", nmea_source, "if (*line_len == 0U)"),
    ("sle_team_nmea.c", nmea_source, "return SLE_TEAM_ERR_FORMAT;"),
    ("ws63_team_status_led.c", status_led_source, "g_status_led_breathe_idle"),
    ("ws63_team_status_led.c", status_led_source, "status_led_apply_scaled"),
    ("ws63_team_status_led.c", status_led_source, "ws63_team_status_led_hold_low"),
    ("ws63_team_status_led.c", status_led_source, "Diagnostic mode: send one all-black frame"),
    ("ws63_ws2812.c", ws2812_source, "ws63_ws2812_set_rgb"),
    ("ws63_ws2812.c", ws2812_source, "ws63_ws2812_hold_low"),
    ("ws63_ws2812.c", ws2812_source, "ws63_ws2812_encode_frame"),
    ("ws63_ws2812.c", ws2812_source, "#define WS63_WS2812_T0H_NS 350U"),
    ("ws63_ws2812.c", ws2812_source, "#define WS63_WS2812_RESET_US 320U"),
    ("ws63_ws2812.c", ws2812_source, "#define WS63_WS2812_PIN_DRIVE PIN_DS_7"),
    ("ws63_ws2812.c", ws2812_source, "WS2812B-XF01/W uses the common WS2812 GRB wire order"),
    ("ws63_ws2812.c", ws2812_source, "Keep the bus quiet"),
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

for bad_ws2812_token in [
    "WS63_WS2812_FRAME_REPEATS",
    "WS63_WS2812_RECALIBRATE_EACH_FRAME",
    "ws63_ws2812_prepare_pin_low",
    "PIN_DS_3",
]:
    reject_text("ws63_ws2812.c", ws2812_source, bad_ws2812_token)
for bad_status_token in [
    "color_cached",
    "last_red",
    "last_green",
    "last_blue",
]:
    reject_text("ws63_team_status_led.c", status_led_source, bad_status_token)

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
if app_lines > 2700 or node_lines > 2200:
    raise SystemExit(f"post-build guard failed: minimal sources grew too large app={app_lines} node={node_lines}")

print(f"post-build guard passed: {VERSION} minimal networking app_lines={app_lines} node_lines={node_lines}")
'''


def main(argv: list[str] | None = None) -> int:
    root = repo_root()
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default=os.environ.get("UBUNTU_HOST", "192.168.6.5"))
    parser.add_argument("--port", type=int, default=int(os.environ.get("UBUNTU_PORT", "22")))
    parser.add_argument("--user", default=os.environ.get("UBUNTU_USER", "owen"))
    parser.add_argument("--password", default=os.environ.get("UBUNTU_PASS", ""))
    parser.add_argument("--sdk", default=os.environ.get("UBUNTU_SDK", "/home/owen/workspace/bearpi-pico_h3863"))
    parser.add_argument("--jobs", default=os.environ.get("BUILD_JOBS", "4"))
    parser.add_argument("--self-id", default="1")
    parser.add_argument("--output", default=str(root / LOCAL_OUT_REL))
    args = parser.parse_args(argv)

    sdk = args.sdk.rstrip("/")
    remote_proto = posixpath.join(sdk, REMOTE_PROTO_REL)
    remote_app = posixpath.join(sdk, REMOTE_APP_REL)
    remote_pkg = posixpath.join(sdk, REMOTE_PKG_REL)
    config_path = posixpath.join(sdk, CONFIG_REL)
    local_output = Path(args.output)
    archive_output = reserve_unique_path(versioned_output_path(local_output, VERSION))

    log("WS63 Ubuntu Paramiko build")
    log(
        f"profile:    {VERSION} unified runtime role "
        "(minimal leader/member/relay networking)"
    )
    log(f"host:       {args.user}@{args.host}:{args.port}")
    log(f"sdk:        {sdk}")
    log(f"archive:    {archive_output}")
    log(f"latest:     {local_output}")

    with tempfile.TemporaryDirectory(prefix="ws63_remote_build_") as tmp_name:
        tmp = Path(tmp_name)
        include_tgz = tmp / "include.tgz"
        src_tgz = tmp / "src.tgz"
        app_tgz = tmp / "sle_team_network.tgz"
        log("[local] packing source archives")
        make_archive(root / "include", "include", include_tgz)
        make_archive(root / "src", "src", src_tgz)
        make_archive(root / "xc" / "ws63_team_network", "sle_team_network", app_tgz)

        client = paramiko.SSHClient()
        client.set_missing_host_key_policy(paramiko.AutoAddPolicy())
        client.connect(
            args.host,
            port=args.port,
            username=args.user,
            password=args.password or None,
            timeout=20,
            banner_timeout=20,
            auth_timeout=20,
        )
        try:
            sftp = client.open_sftp()
            remote_tmp = f"/tmp/ws63_remote_build_{os.getpid()}_{int(time.time())}"
            run_remote(
                client,
                f"set -e; test -f {quote(config_path)}; mkdir -p {quote(remote_tmp)} {quote(remote_proto)} {quote(posixpath.dirname(remote_app))}",
                "preflight",
            )
            upload_file(sftp, include_tgz, posixpath.join(remote_tmp, "include.tgz"))
            upload_file(sftp, src_tgz, posixpath.join(remote_tmp, "src.tgz"))
            upload_file(sftp, app_tgz, posixpath.join(remote_tmp, "sle_team_network.tgz"))

            run_remote(
                client,
                "set -e; "
                f"rm -rf {quote(posixpath.join(remote_proto, 'include'))} "
                f"{quote(posixpath.join(remote_proto, 'src'))} {quote(remote_app)}; "
                f"mkdir -p {quote(remote_proto)} {quote(posixpath.dirname(remote_app))}; "
                f"tar -xzf {quote(posixpath.join(remote_tmp, 'include.tgz'))} -C {quote(remote_proto)}; "
                f"tar -xzf {quote(posixpath.join(remote_tmp, 'src.tgz'))} -C {quote(remote_proto)}; "
                f"tar -xzf {quote(posixpath.join(remote_tmp, 'sle_team_network.tgz'))} -C {quote(posixpath.dirname(remote_app))}",
                "sync source",
            )

            lvgl_patch = posixpath.join(remote_app, "third_party/lvgl-patches/lv8.3.11-ws63-c89-rect.patch")
            run_remote(
                client,
                "set -e; "
                f"LVGL_PATCH={quote(lvgl_patch)}; REMOTE_APP={quote(remote_app)}; "
                'if [ -f "$LVGL_PATCH" ]; then '
                'cd "$REMOTE_APP/third_party/lvgl"; '
                'if grep -q "lv_area_t center_coords;" src/draw/sw/lv_draw_sw_rect.c && '
                'grep -q "bool mask_any_center = false;" src/draw/sw/lv_draw_sw_rect.c; then '
                'echo "LVGL patch already present in source"; '
                'elif git apply --unidiff-zero --check "$LVGL_PATCH"; then '
                'git apply --unidiff-zero "$LVGL_PATCH"; '
                'elif git apply --unidiff-zero --reverse --check "$LVGL_PATCH"; then '
                'echo "LVGL patch already applied"; '
                'else echo "LVGL patch check failed: $LVGL_PATCH" >&2; exit 1; fi; fi',
                "apply lvgl patch",
            )

            config_py = posixpath.join(remote_tmp, "configure.py")
            guard_py = posixpath.join(remote_tmp, "post_build_guard.py")
            with sftp.file(config_py, "w") as f:
                f.write(build_config_script())
            with sftp.file(guard_py, "w") as f:
                f.write(post_build_guard_script())

            run_remote(client, f"python3 {quote(config_py)} {quote(config_path)} {quote(args.self_id)}", "configure kconfig")
            run_remote(client, f"cd {quote(sdk)} && python3 build.py -c ws63-liteos-app -j{quote(str(args.jobs))}", "build firmware", timeout=3600)
            run_remote(client, f"python3 {quote(guard_py)} {quote(sdk)}", "post-build guard")

            download_file(sftp, remote_pkg, archive_output)
            pkg_bytes = archive_output.read_bytes()
            if VERSION.encode("ascii") not in pkg_bytes:
                raise RuntimeError(f"downloaded package does not contain {VERSION}: {archive_output}")
            if archive_output != local_output:
                shutil.copy2(archive_output, local_output)
                log(f"[latest] updated {local_output}")
            log(f"[done] package size={archive_output.stat().st_size} contains_{VERSION}=True archive={archive_output}")
            run_remote(client, f"rm -rf {quote(remote_tmp)}", "cleanup")
        finally:
            client.close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
