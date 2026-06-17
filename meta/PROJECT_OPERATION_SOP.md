# Project Operation SOP

## v4.5.0-Minimal Networking Rewrite

Current code work is tracked in:

```text
docs/v4/ws63_network_rewrite_task_book.md
```

This rewrite removes the old staged/frozen topology contract and keeps only the minimal direct/member/relay flow. Use the current minimal status fields and do not treat the retired route-metrics contract as active guidance.

## v4.4.94 Relay Swap Hysteresis Entry Point

Current code work is tracked in:

```text
versions\v4.4.94\VERSION.md
```

This version keeps the dynamic relay budget and adds stable relay optimization: a non-relay node can replace the worst active relay only when its RSSI is at least 8 dB stronger for 30 seconds while the topology is otherwise stable.

For v4.4.94 in this session, remote compile was later run after the user explicitly re-enabled compile work. Hardware flash/burn is still skipped unless the user explicitly asks for it.

## v4.4.93 Dynamic Relay Historical Entry Point

The previous dynamic relay sizing work is tracked in:

```text
versions\v4.4.93\VERSION.md
```

This version changes relay sizing policy. Treat `cfg direct 1` as a small-board relay-forcing test setup, not as the normal 30-member deployment profile. Normal scaling analysis should start from the default leader direct capacity and the dynamic relay-sizing status fields.

For v4.4.93 in its session, firmware build and hardware burn were intentionally skipped because only four boards were available.

## v4.4.92 Four-Board Historical Entry Point

For the completed v4.4.92 four-board validation objective, do not rediscover commands.
Use the version runbook first:

```text
versions\v4.4.92\FLASH_AND_FOUR_BOARD_TEST.md
```

That runbook records:

- Firmware package path and expected `v4.4.92` package guard.
- Parallel flash command for `COM16,COM13,COM17,COM18`.
- COM16 leader, COM13 relay candidate, COM17/COM18 member role assignment.
- Leader `cfg direct 1` setup for one direct member and two relayed members.
- Required member reboot, relay reboot, child relay self-election, and recovery-policy checks.
- Required live logs under `logs\live\v4.4.92_four_board_com16_leader_<timestamp>`.

Current policy: do not flash blindly. Flash only when explicitly continuing the
hardware validation run, and always keep the burn/test logs as proof.

## Current Flash Flow Template

For WS63 v4 work, use `scripts/flash/ws63_flash_multi.ps1` instead of rebuilding burn commands by hand.

This section records the standard flash command shape only. It does not mean v4.4.94 has been flashed; v4.4.94 burn was intentionally not run in the relay swap hysteresis session.

Sequential flash:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash\ws63_flash_multi.ps1 `
  -Ports COM16 `
  -ExpectedVersion v4.4.94 `
  -WaitTimeout 45 `
  -ManualRetryTimeout 0
```

Parallel flash:

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\scripts\flash\ws63_flash_multi.ps1 `
  -Ports COM16,COM13,COM17,COM18 `
  -ExpectedVersion v4.4.94 `
  -Parallel `
  -ParallelStartDelayMs 1200 `
  -WaitTimeout 45 `
  -ManualRetryTimeout 0
```

The verified burn flow is:

```text
software-reset-only + single reboot command + post-package handshake
```

Success must be proven by burn output and version guard:

```text
Establishing ymodem session...
Done. Reseting device...
expected: v4.4.94
```

Do not ask for manual reset during normal v4.4.94 flashing. The current boards can enter the loader through the serial `reboot` command. Use `ManualRetryTimeout=0` so failures are real evidence instead of hidden manual intervention.

Do not treat a port as flashed only because the package transfer command was attempted. If there is no boot handshake or the expected version is missing from the package, record the port as blocked and continue debugging from logs.

For multi-board flashing, prefer `-ParallelStartDelayMs 1200` or higher. This keeps the flash jobs parallel but staggers reset/handshake startup so multiple CH340 boards do not all enter the boot handshake at exactly the same moment.

Four-board validation after flashing:

```powershell
$ts = Get-Date -Format 'yyyyMMdd_HHmmss'
$logDir = "<repo-root>\logs\live\v4.4.92_four_board_com16_leader_$ts"
python .\automation\ws63\tools\ws63_four_board_relay_test.py `
  --leader-port COM16 `
  --relay-port COM13 `
  --child1-port COM17 `
  --child2-port COM18 `
  --expected-fw v4.4.92 `
  --team-id 1 `
  --channel 17 `
  --direct-cap 1 `
  --initial-drain-s 2 `
  --cmd-timeout-s 25 `
  --state-timeout-s 90 `
  --route-timeout-s 120 `
  --offline-timeout-s 20 `
  --boot-timeout-s 75 `
  --failover-timeout-s 120 `
  --poll-interval-s 1 `
  --log-dir $logDir
```

The four-board tool defaults to clean-start: it sends `cfg clear`, reboots each board, verifies `runtimeConfigured=false`, then configures COM16 as leader and COM13/COM17/COM18 as members. Use `--no-clean-start` only for a deliberate diagnostic replay.

这份文档是 `sle_mesh` 工程的强制作业入口。以后每次改代码、改脚本、改 WebUI、改固件参数、远程编译或烧录前，都先读这里；如果流程变化，必须同步更新这里和本次版本记录。

## 1. 每次开工前

1. 先读本文件，再读当前最新版本目录的 `VERSION.md` 和 `MANIFEST.md`。
2. 执行 `git status --short --branch`，确认当前分支、脏文件和未跟踪产物。
3. 确认最新版本号：优先看 `README.md` 的“当前版本”和 `versions/README.md` 顶部。
4. 不要回退别人或上一次调试留下的改动；如果改动冲突，先停下来确认。
5. 新工作默认在 `line/` 前缀分支上完成，除非用户明确要求别的分支。

## 2. 版本管理硬规则

每次代码行为变化都必须升版本，不能继续停在旧版本号。

默认规则：

1. 小修、文档流程、参数同步、bugfix：补丁号加一，例如 `v4.4` -> `v4.4.1`。
2. 新功能或跨模块同步：小版本加一，例如 `v4.4.1` -> `v4.5`。
3. 重大架构变化或不可兼容协议：主版本加一。
4. 不覆盖旧版本目录；新增 `versions/<new-version>/`。
5. 固件代码变更时，必须同步更新固件版本字符串、启动日志可见版本、屏幕可见版本。

每次版本记录至少包含：

1. `versions/<version>/VERSION.md`：本版定位、解决了什么、已知限制。
2. `versions/<version>/MANIFEST.md`：改了哪些文件、如何验证、构建产物路径、包大小、时间。
3. `versions/README.md`：新版本放到顶部。
4. `README.md`：当前版本和关键入口同步更新。
5. 如果烧录或硬件问题流程变化，更新本 SOP。

## 3. 远程 Ubuntu 编译

WS63 固件优先使用局域网 Ubuntu 编译机，不使用本地虚拟机作为默认路径。

当前默认编译机：

```text
Host: 192.168.6.5
User: owen
SDK: /home/owen/workspace/bearpi-pico_h3863
Project app: /home/owen/workspace/bearpi-pico_h3863/application/samples/products/sle_team_network
Protocol copy: /home/owen/workspace/bearpi-pico_h3863/third_party/sle_mesh
```

标准编译命令：

```sh
UBUNTU_HOST=192.168.6.5 \
UBUNTU_USER=owen \
UBUNTU_PASS='<set locally, do not commit secrets>' \
UBUNTU_SDK=/home/owen/workspace/bearpi-pico_h3863 \
BUILD_JOBS=4 \
scripts/build/ws63_build_v4_ubuntu.sh unified
```

输出固件固定记录为：

```text
<repo-root>\output_from_vm\team_network_v4_unified_runtime_role\ws63-liteos-app_v4_unified_all.fwpkg
```

如果本机缺少 `sshpass` 或 `rsync`，允许用 Python `paramiko` 做同等步骤，但版本记录里必须写清楚使用了 fallback。

## 4. 自动烧录

COM16 成功路径：

```powershell
python <repo-root>\automation\ws63\tools\ws63_auto_burn.py `
  -p COM16 `
  -b 115200 `
  --software-reset-only `
  --reset-command reboot `
  --reset-command-fallback reset `
  --reset-command-delay 0.3 `
  --reset-command-retries 2 `
  --reset-command-retry-gap 0.2 `
  <repo-root>\output_from_vm\team_network_v4_unified_runtime_role\ws63-liteos-app_v4_unified_all.fwpkg
```

注意事项：

1. 这块板子不能假设有 RTS/DTR 自动复位硬件。
2. `--software-reset-only` 依赖旧固件串口能响应 `reboot` 或 `reset`。
3. 如果旧固件无响应，需要用户手动按 `RESET/RST` 配合进入烧录。
4. 烧录成功后必须在版本记录写清楚端口、命令、是否手动 reset、固件包路径。

## 5. 屏幕和硬件固定结论

当前确认的 ST7789 1.14 寸屏参数：

```text
SCLK/SCL: GPIO6
CS: GPIO7
MOSI/SDA: GPIO8
DC/RS: GPIO9
RESET: GPIO13
BLK/backlight: 硬件默认开启，固件不要控制 GPIO11
Logical size: 240x135
X offset: 40
Y offset: 53
MADCTL: 0x60
SPI: software SPI mode 0
```

历史坑：

1. `135x240 + offset 52,40` 会出现局部变色、周围花屏。
2. BLK/GPIO11 不再写固件控制逻辑，避免误判背光。
3. 屏幕问题完整记录见 `versions/v4.4/ST7789_DISPLAY_FIX.md`。

## 6. 批量串口配置

30 个节点批量部署时，优先使用串口或 WebSerial 一键配置，不要逐个连接 WiFi。

串口参数：

```text
115200 8N1
```

常用命令：

```text
cfg status
cfg leader now <team> <channel>
cfg member now <leader_suffix_hex> <team> <channel>
cfg apply
cfg clear
cfg reboot
```

Windows helper：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM7 -Mode leader -Team 7 -Channel 33
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM8 -Mode member -LeaderSuffix 9A2F -Team 7 -Channel 33
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM8 -Mode status
```

域名 WebUI 的 `Settings -> One-click node config` 使用 WebSerial 读取 `[cfg-json]` 并显示串口日志。

## 7. 必跑验证

按影响范围执行，能多跑就多跑。

文档和通用检查：

```sh
git diff --check
```

外部 WebUI 改动：

```sh
npm --prefix webui test
npm --prefix webui run build
```

固件业务代码、Kconfig、显示、串口、SLE、WiFi 改动：

```sh
UBUNTU_HOST=192.168.6.5 \
UBUNTU_USER=owen \
UBUNTU_PASS='<set locally, do not commit secrets>' \
UBUNTU_SDK=/home/owen/workspace/bearpi-pico_h3863 \
BUILD_JOBS=4 \
scripts/build/ws63_build_v4_ubuntu.sh unified
```

如果修改协议核心，补跑对应 C 侧或 Python 仿真测试，并把命令写入 `MANIFEST.md`。

## 8. 回退方法

1. 先查 `versions/README.md`，找到目标版本。
2. 读目标版本的 `VERSION.md` 和 `MANIFEST.md`，确认它对应的固件包、提交和验证记录。
3. 如果只是恢复板子，优先烧录旧 `.fwpkg`。
4. 如果要恢复代码，使用 Git 分支/提交回退，不要手动覆盖多个文件。
5. 回退后仍要记录一次新版本，说明为什么回退、回退到哪个版本、验证结果是什么。

## 9. 本文件维护规则

1. 任何人发现自动烧录、远程编译、版本管理、屏幕参数、串口配置流程和本文件不一致，必须马上更新本文件。
2. 本文件是长期 SOP，不写临时草稿；临时过程写入对应 `versions/<version>/`。
3. 如果下一次任务只改文档，也要遵守版本记录规则，因为文档会影响后续烧录和回退判断。
