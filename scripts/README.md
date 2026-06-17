# Scripts

脚本按用途隔离，根目录不再直接堆放可执行脚本。

## 分类

- [build/](build/)：固件构建脚本，包括远程 Ubuntu、VM、WSL 构建入口。
- [flash/](flash/)：烧录脚本，包括单板和多串口烧录。
- [serial/](serial/)：串口运行时配置脚本。
- [sim/](sim/)：协议和 relay 行为仿真脚本。
- [test/](test/)：测试总入口和系统级编排入口。
- [review/](review/)：文档/代码审查辅助脚本。

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
