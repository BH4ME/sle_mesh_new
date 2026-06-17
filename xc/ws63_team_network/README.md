# WS63 Team Network Firmware

这是当前 WS63 SLE team network 板端固件工程。

## 当前版本

- 固件版本：`v4.4.134`
- 仓库整理记录：`v4.4.134`
- 版本说明：[../../versions/v4.4.134/VERSION.md](../../versions/v4.4.134/VERSION.md)
- 仓库结构说明：[../../docs/repository_layout.md](../../docs/repository_layout.md)

## 功能

- 统一固件包：leader/member 角色运行时配置。
- 板端 SoftAP + HTTP WebUI。
- 串口 `cfg` 命令配置角色、team、channel 和 leader suffix。
- leader/member/relay 自适应连接与中继。
- ST7789 显示在线状态和成员事件。
- WS2812、蜂鸣器、GPS pinmap、HTTP API 等板级能力。

## 串口命令

```text
cfg status
cfg leader now <team> <channel>
cfg member now <leader_suffix_hex> <team> <channel>
cfg apply
cfg clear
cfg reboot
reboot
reset
```

Windows 辅助脚本：

```powershell
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM16 -Mode leader -Team 7 -Channel 33
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM13 -Mode member -LeaderSuffix 9A2F -Team 7 -Channel 33
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM13 -Mode status
```

## 板端 WebUI

```text
SSID: SLE-TEAM-V4-XXXX
Password: 123456789
URL: http://192.168.43.1/
```

常用 API：

- `GET /api/status`
- `GET /api/power`
- `GET /api/nodes`
- `GET /api/events`
- `GET /api/pending`
- `GET /api/config/status`
- `GET /api/config/leader?team=1&channel=17&now=1`
- `GET /api/config/member?leader=C7E9&team=1&channel=17&now=1`
- `GET /api/config/apply`
- `GET /api/config/clear`
- `GET /api/config/reboot`
- `GET /api/pairing?action=start|stop|approve&id=...&relay=0|1`
- `GET /api/member/select?team=...&leader=...&channel=...`
- `GET /api/member/leave`
- `GET /api/factory-reset`

## 构建

推荐使用仓库根目录的远程构建脚本：

```sh
scripts/build/ws63_build_v4_ubuntu.sh unified
```

输出固件包：

```text
output_from_vm/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg
```

## 目录

- `src/`：板端应用、显示、LED 等实现。
- `sle_uart_client/`：leader/client/central 侧 SLE UART 适配。
- `sle_uart_server/`：member/server/peripheral 侧 SLE UART 适配。
- `third_party/lvgl-patches/`：WS63 LVGL 适配补丁。
- `CMakeLists.txt`、`Kconfig`：SDK 工程集成入口。
