#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
AUTO_BURN_TOOL="$ROOT_DIR/automation/ws63/tools/ws63_auto_burn.py"
BURN_TOOL="${BURN_TOOL:-$AUTO_BURN_TOOL}"
FW_ROOT="${FW_ROOT:-$ROOT_DIR/output_from_vm}"
AUTO_RESET="${AUTO_RESET:-1}"
RESET_COMMAND="${RESET_COMMAND:-reboot}"
RESET_COMMAND_FALLBACK="${RESET_COMMAND_FALLBACK:-reset}"
RESET_COMMAND_DELAY="${RESET_COMMAND_DELAY:-0.3}"
RESET_COMMAND_RETRIES="${RESET_COMMAND_RETRIES:-2}"
RESET_COMMAND_RETRY_GAP="${RESET_COMMAND_RETRY_GAP:-0.2}"
AUTO_RESET_MODE="${AUTO_RESET_MODE:-software-only}"
RESET_CONTROL_SEQUENCE="${RESET_CONTROL_SEQUENCE:-rts=0,dtr=0:0.05;rts=0,dtr=1:0.12;rts=0,dtr=0:0.05}"
SKIP_RESET_IF_ROM_ACTIVE="${SKIP_RESET_IF_ROM_ACTIVE:-1}"
ROM_PREFLIGHT_TIMEOUT="${ROM_PREFLIGHT_TIMEOUT:-1.0}"
EXPECTED_FW_VERSION="${EXPECTED_FW_VERSION:-v4.4.142}"
NO_CONFIRM="${WS63_FLASH_NO_CONFIRM:-0}"

usage() {
  cat <<'USAGE'
Usage:
  scripts/flash/ws63_flash_team.sh [--yes] leader [port]
  scripts/flash/ws63_flash_team.sh [--yes] member [port]
  scripts/flash/ws63_flash_team.sh [--yes] unified [port]

Defaults:
  leader port: /dev/tty.usbserial-10
  member port: /dev/tty.usbserial-110

Environment:
  BURN_TOOL=/path/to/burn
  FW_ROOT=/path/to/output_from_vm
  AUTO_RESET=1|0
  AUTO_RESET_MODE=software-only|hybrid
  WS63_FLASH_NO_CONFIRM=1|0
  RESET_COMMAND=reboot
  RESET_COMMAND_FALLBACK=reset
  RESET_COMMAND_DELAY=0.3
RESET_COMMAND_RETRIES=2
RESET_COMMAND_RETRY_GAP=0.2
RESET_CONTROL_SEQUENCE='rts=0,dtr=0:0.05;rts=0,dtr=1:0.12;rts=0,dtr=0:0.05'
SKIP_RESET_IF_ROM_ACTIVE=1
ROM_PREFLIGHT_TIMEOUT=1.0
EXPECTED_FW_VERSION=v4.4.142

The script prints role, port, and firmware path, then asks for an exact
confirmation before it runs the burn tool.
Use --yes or WS63_FLASH_NO_CONFIRM=1 for non-interactive runs.

By default this script uses automation/ws63/tools/ws63_auto_burn.py in software-only
mode, which sends serial CLI reset commands and does not depend on RTS/DTR wiring.
Set AUTO_RESET_MODE=hybrid to also toggle DTR/RTS before burn handshake.
Set AUTO_RESET=0 to keep the old manual-reset flow.
USAGE
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    -y|--yes)
      NO_CONFIRM=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    --)
      shift
      break
      ;;
    -*)
      echo "Unknown option: $1" >&2
      usage >&2
      exit 2
      ;;
    *)
      break
      ;;
  esac
done

role="${1:-}"
port_arg="${2:-}"
if [[ $# -gt 2 ]]; then
  echo "Too many arguments." >&2
  usage >&2
  exit 2
fi

case "$role" in
  leader)
    default_port="/dev/tty.usbserial-10"
    firmware="$FW_ROOT/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg"
    fallback_firmware="$FW_ROOT/team_network_leader_serial_led/ws63-liteos-app_leader_all.fwpkg"
    ;;
  member)
    default_port="/dev/tty.usbserial-110"
    firmware="$FW_ROOT/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg"
    fallback_firmware="$FW_ROOT/team_network_member_serial_led/ws63-liteos-app_member_all.fwpkg"
    ;;
  unified)
    default_port="/dev/tty.usbserial-10"
    firmware="$FW_ROOT/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg"
    fallback_firmware="$firmware"
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

port="${port_arg:-$default_port}"

echo "WS63 team flash confirmation"
echo "repo:     $ROOT_DIR"
echo "role:     $role"
echo "port:     $port"
echo "firmware: $firmware"
echo "expected: $EXPECTED_FW_VERSION"
echo "auto rst: $AUTO_RESET"
echo "auto mode:$AUTO_RESET_MODE"
echo "rom pre:  $SKIP_RESET_IF_ROM_ACTIVE timeout=$ROM_PREFLIGHT_TIMEOUT"
echo

if [[ ! -x "$BURN_TOOL" ]]; then
  echo "Burn tool is not executable: $BURN_TOOL" >&2
  exit 1
fi

if [[ ! -e "$port" ]]; then
  echo "Serial port does not exist: $port" >&2
  echo "Current likely serial ports:" >&2
  ls -1 /dev/tty.* /dev/cu.* 2>/dev/null | sed -n '/usb\|wch\|serial\|SLAB\|UART\|modem/Ip' >&2 || true
  exit 1
fi

if [[ ! -f "$firmware" ]]; then
  if [[ -f "$fallback_firmware" ]]; then
    echo "Unified firmware package not found, using previous serial LED package:"
    echo "fallback: $fallback_firmware"
    firmware="$fallback_firmware"
  else
    echo "Firmware package does not exist: $firmware" >&2
    echo "Fallback package does not exist: $fallback_firmware" >&2
    exit 1
  fi
fi

if [[ -n "$EXPECTED_FW_VERSION" ]]; then
  if ! grep -a -q "$EXPECTED_FW_VERSION" "$firmware"; then
    echo "Firmware package does not contain expected version: $EXPECTED_FW_VERSION" >&2
    echo "Refusing to flash stale package: $firmware" >&2
    echo "Set EXPECTED_FW_VERSION= to override only when intentionally flashing an older image." >&2
    exit 1
  fi
fi

expected="flash $role"
if [[ "$NO_CONFIRM" == "1" ]]; then
  echo "Non-interactive mode: skip manual confirmation."
else
  printf "Type '%s' to continue: " "$expected"
  read -r answer
  if [[ "$answer" != "$expected" ]]; then
    echo "Cancelled."
    exit 1
  fi
fi

burn_args=(-p "$port" -b 115200)
if [[ "$BURN_TOOL" == "$AUTO_BURN_TOOL" ]]; then
  if [[ "$AUTO_RESET" == "0" ]]; then
    burn_args+=(--no-auto-reset)
    echo "Starting burn with auto reset disabled. Press RESET/RST if the tool waits for reset."
  else
    case "$AUTO_RESET_MODE" in
      software|software-only)
        burn_args+=(--software-reset-only)
        ;;
      hybrid)
        burn_args+=(--control-sequence "$RESET_CONTROL_SEQUENCE")
        ;;
      *)
        echo "Unknown AUTO_RESET_MODE: $AUTO_RESET_MODE (expected software-only|hybrid)" >&2
        exit 2
        ;;
    esac
    burn_args+=(
      --reset-command "$RESET_COMMAND"
      --reset-command-fallback "$RESET_COMMAND_FALLBACK"
      --reset-command-delay "$RESET_COMMAND_DELAY"
      --reset-command-retries "$RESET_COMMAND_RETRIES"
      --reset-command-retry-gap "$RESET_COMMAND_RETRY_GAP"
    )
    if [[ "$SKIP_RESET_IF_ROM_ACTIVE" != "0" ]]; then
      burn_args+=(--skip-reset-if-rom-active --rom-preflight-timeout "$ROM_PREFLIGHT_TIMEOUT")
    fi
    echo "Starting burn with auto reset enabled."
    echo "If this board still waits for reset, press RESET/RST manually; boards with a BOOT key may need BOOT + RESET."
  fi
else
  if [[ "$AUTO_RESET" != "0" ]]; then
    echo "AUTO_RESET requested, but custom BURN_TOOL does not support project auto reset: $BURN_TOOL" >&2
    echo "Falling back to the custom burn tool; press RESET/RST if it waits for reset." >&2
  fi
  echo "Starting burn with custom burn tool."
fi
exec "$BURN_TOOL" "${burn_args[@]}" "$firmware"
