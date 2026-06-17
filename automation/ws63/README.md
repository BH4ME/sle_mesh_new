# WS63 Automation

`automation/ws63/` 是 WS63 自动化工具链，负责烧录编排、串口预检、角色绑定和多板 relay 测试。它与固件源码、仿真脚本和通用工具分开管理。

## 目录结构

- `tools/`
  - `ws63_auto_burn.py`：自动复位烧录工具，支持串口 `reboot` 和 DTR/RTS 控制序列。
  - `ws63_flash_bind_team.py`：批量烧录并绑定角色。
  - `ws63_four_board_relay_test.py`：四板 leader/member/relay 场景测试。
  - `ws63_link_cycle_test.py`：member 生命周期、重启、leave/rejoin 测试。
  - `ws63_relay_cycle_test.py`：relay 重启和 relay 自举场景测试。
  - `ws63_remote_build_v4.py`：Python/Paramiko 远程构建入口。
  - `ws63_serial_preflight.py`：串口测试前置检查。
- `scripts/`
  - `ws63_test_system.sh`：自动化总入口。
- `tests/`
  - 自动化工具单元测试。

## 与其他目录的边界

- 固件构建脚本：`scripts/build/`
- 烧录脚本：`scripts/flash/`
- 串口配置脚本：`scripts/serial/`
- 仿真脚本：`scripts/sim/`
- 通用仿真工具：`tools/sle_team_python_sim.py`

## 常用命令

```sh
python -m unittest discover -s automation/ws63/tests -t .
bash automation/ws63/scripts/ws63_test_system.sh --with-relay-cycle --ports COM16,COM13,COM17
```

```powershell
python automation/ws63/tools/ws63_auto_burn.py -p COM16 -b 115200 --software-reset-only output_from_vm/team_network_v4_unified_runtime_role/ws63-liteos-app_v4_unified_all.fwpkg
```
