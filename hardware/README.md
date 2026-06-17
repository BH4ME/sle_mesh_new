# Hardware

这个目录用于存放 SLE 项目的硬件资料和可制造文件。硬件资料与固件源码、自动化脚本分开管理，避免仓库根目录堆放临时模型或草稿文件。

## 目录

- [enclosures/](enclosures/)：3D 打印外壳、STEP/STL、预览图和源模型脚本。
- `boards/`：预留给 PCB、Gerber、BOM、装配图和板卡 3D 模型。
- `schematics/`：预留给原理图、接口定义和硬件审查记录。

## 当前硬件资料

- [boards/sle-main-board/v3.3](boards/sle-main-board/v3.3/)：2026-06-16 SLE 主板 PCB PDF 和板卡预览图。
- [schematics/sle-main-board/v3.3](schematics/sle-main-board/v3.3/)：2026-06-16 SLE 主板原理图 PDF。

## 版本规则

- 每个可制造硬件版本放在独立目录，例如 `hardware/enclosures/sle-pcb-enclosure/v1.1.4/`。
- 每个版本至少包含 `README.md` 和 `MANIFEST.md`。
- STEP/STL/PNG 等二进制或大型制造文件按类型放入 `step/`、`stl/`、`preview/`。
- 草稿、临时预览、审查截图和未确认模型不要直接放仓库根目录，确认可发布后再整理进本目录。
