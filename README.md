# sle_mesh_new

WS63 SLE mesh firmware, board Web UI, browser Web UI, flashing tools, debug automation, and CAD STL assets.

Current packaged line: `v4.5.56-minimal`

## What Is Included

| Area | Path | Purpose |
| --- | --- | --- |
| WS63 firmware app | `xc/ws63_team_network/` | Unified leader/member/relay firmware, ST7789/LVGL display task, WS2812 status LED, GPS/location hooks, battery/ADC hooks, and board HTTP service. |
| Shared protocol/core | `include/`, `src/` | Portable SLE team packet, node state machine, CLI, location, relay optimizer, NMEA parser, and Web API code. |
| Web UI | `webui/` | Browser/board Web UI source and API contract tests. |
| Flash tools | `scripts/flash/`, `tools/xf_burn_tools/`, `automation/ws63/tools/ws63_auto_burn.py` | Windows and Python flashing workflows for WS63 `.fwpkg` images. |
| Build tools | `scripts/build/`, `automation/ws63/tools/ws63_remote_build_v4.py` | Local/remote SDK build wrappers and post-build guards. |
| Debug/test automation | `automation/ws63/`, `scripts/sim/`, `examples/` | Serial probes, exact-version audit, relay recovery hardware tests, double-reset tests, simulator and C regressions. |
| Hardware docs | `hardware/`, `docs/` | Board/enclosure documentation, schematics notes, task book, and repository docs. |
| CAD STL models | `cad/stl/` | Only the latest three versioned enclosure STL model sets. |
| Release firmware | `release/firmware/` | Latest known built package for direct flashing. |
| Release evidence | `release/evidence/` | Compact JSON summaries for hardware proof runs. |

## Current Hardware-Proven Status

`v4.5.56-minimal` is the current proof line with same-firmware compatibility fingerprint `SLE_TEAM_FW_COMPAT 0x0556U`.

The exact-version 8-board relay recovery run passed:

- Summary: `release/evidence/v4_5_56_exact8_relay_recovery_summary.json`
- Leader: `COM51`, route `166`, suffix `227F`
- Members: `COM32/175`, `COM40/127`, `COM42/81`, `COM47/181`, `COM50/158`, `COM52/182`, `COM58/37`
- Initial relay: `COM42/81`
- Replacement relay after relay reboot: `COM52/182`
- Old relay `81` returned as a non-relay member under relay `182`
- Final relay set: `[182]`
- No `Oops`, exception, NMI, or runtime reboot markers were found in the proof logs

The same board pool also printed SoftAP, HTTP, LVGL backend, and ST7789 startup logs. AP client visibility scanning was intentionally not performed for this proof pass. WS2812 color/brightness has no readback channel, so final LED appearance still requires physical observation on real boards.

## Flash The Included Firmware

Firmware package:

```text
release/firmware/ws63-liteos-app_v4_unified_all_v4.5.56-minimal.fwpkg
```

Windows multi-port flash:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash/ws63_flash_multi.ps1 `
  -Ports COM32,COM40,COM42,COM47,COM50,COM51,COM52,COM58 `
  -Parallel `
  -Firmware release\firmware\ws63-liteos-app_v4_unified_all_v4.5.56-minimal.fwpkg `
  -ExpectedVersion v4.5.56-minimal
```

Single-port Python burn:

```powershell
python automation\ws63\tools\ws63_auto_burn.py `
  -p COM51 `
  -b 115200 `
  --software-reset-only `
  --reset-command reboot `
  release\firmware\ws63-liteos-app_v4_unified_all_v4.5.56-minimal.fwpkg
```

Audit exact versions and idle state:

```powershell
python automation\ws63\tools\ws63_audit_versions_and_idle_bad.py `
  --ports COM32 COM40 COM42 COM47 COM50 COM51 COM52 COM58 `
  --expected-fw v4.5.56-minimal
```

## Run Relay Recovery Hardware Test

```powershell
python automation\ws63\tools\ws63_multi_board_relay_recovery_test.py `
  --leader-port COM51 `
  --member-ports COM32 COM40 COM42 COM47 COM50 COM52 COM58 `
  --expected-fw v4.5.56-minimal `
  --team-id 159 `
  --channel 189 `
  --direct-cap 3 `
  --log-dir logs\hardware\v4_5_56_exact8_relay_recovery
```

## Build Firmware

Remote Ubuntu SDK build:

```powershell
python automation\ws63\tools\ws63_remote_build_v4.py `
  --host <ubuntu-host> `
  --user <ubuntu-user> `
  --password <password>
```

Do not commit local credentials. The build wrapper performs source and package guards for the active proof line.

## Web UI

```powershell
npm --prefix webui install
npm --prefix webui test
npm --prefix webui run build
```

The board HTTP/Web code is in `xc/ws63_team_network/src/ws63_team_http.*` and the browser UI is in `webui/src/`.

## Local Validation

```powershell
python -m unittest discover -s automation/ws63/tests -t .
python -m py_compile automation/ws63/tools/ws63_remote_build_v4.py automation/ws63/tools/ws63_auto_burn.py
bash scripts/sim/simulate_v2.sh --suite=core
git diff --check
```

## Repository Notes

- Full hardware logs, temporary build outputs, local planning files, and large generated CAD formats are intentionally excluded from Git.
- STL files are included under `cad/stl/`; only the latest three versioned model sets are kept.
- LVGL core source needed by the firmware build is vendored under `xc/ws63_team_network/third_party/lvgl/`; demo/media assets are intentionally omitted.
- The repository is intended to be a clean handoff package, not the full local laboratory log archive.
