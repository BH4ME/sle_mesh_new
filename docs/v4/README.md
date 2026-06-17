# V4 Documentation

`docs/v4/` 记录 WS63 模块、ST7789 显示、板端 WebUI、运行时角色配置和 relay 组网验证相关内容。

## 当前主线

- 当前固件版本：`v4.4.129`
- 最新仓库整理记录：`v4.4.129`
- 固件工程：[../../xc/ws63_team_network/](../../xc/ws63_team_network/)
- 硬件原理图：[../../hardware/schematics/sle-main-board/v0.1/](../../hardware/schematics/sle-main-board/v0.1/)
- 远程构建脚本：[../../scripts/build/ws63_build_v4_ubuntu.sh](../../scripts/build/ws63_build_v4_ubuntu.sh)
- 烧录脚本：[../../scripts/flash/](../../scripts/flash/)
- 仿真脚本：[../../scripts/sim/](../../scripts/sim/)

## V4 能力边界

- 所有 WS63 节点烧录同一份统一固件包。
- leader/member 角色通过 WebUI 或串口 `cfg` 命令在运行时配置。
- leader 支持 pending/member 管理、配队窗口、allowlist 和成员在线/离线事件。
- member 可直连 leader，也可在 leader 直连容量受限时通过 relay 转发。
- relay 选择由固件根据连接状态、RSSI 和 relay budget 自适应处理。
- ST7789 显示用于角色、在线状态、成员事件和故障提示。

## 常用入口

```sh
scripts/build/ws63_build_v4_ubuntu.sh unified
scripts/sim/simulate_v2.sh --suite=python --stress=1
python -m unittest discover -s automation/ws63/tests -t .
```

```powershell
powershell -ExecutionPolicy Bypass -File scripts/serial/ws63_serial_cfg.ps1 -Port COM16 -Mode status
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/flash/ws63_flash_multi.ps1 -Ports COM16,COM13,COM17,COM18 -Parallel
```

## 历史记录

详细发布历史见 [../../versions/README.md](../../versions/README.md)。当前仓库结构以 [../repository_layout.md](../repository_layout.md) 和根目录 [../../README.md](../../README.md) 为准。
