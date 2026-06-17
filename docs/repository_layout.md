# Repository Layout

本仓库按“固件、脚本、自动化、硬件、版本记录”分区。

| 路径 | 用途 |
| --- | --- |
| `firmware/` | 固件工程索引和版本边界说明。实际 SDK 工程仍在 `xc/` 下。 |
| `xc/ws63_team_network/` | 当前 WS63 SLE team network 板端固件。 |
| `include/`, `src/` | 可在本地测试和固件中复用的协议/状态机代码。 |
| `examples/` | C 侧回归和演示程序。 |
| `scripts/build/` | 构建脚本。 |
| `scripts/flash/` | 烧录脚本。 |
| `scripts/serial/` | 串口配置脚本。 |
| `scripts/sim/` | 仿真脚本。 |
| `scripts/test/` | 测试总入口。 |
| `automation/ws63/` | WS63 自动化工具、测试和系统编排。 |
| `webui/` | 浏览器 WebUI。 |
| `hardware/` | PCB、原理图、3D 打印外壳等硬件资料。 |
| `versions/` | 仓库/固件/硬件发布记录和验证记录。 |
| `docs/` | 阶段文档和维护说明。 |
| `meta/` | 项目操作 SOP 和维护规则。生成型审查输出不提交。 |

## 本地草稿

`.planning/`、`models/`、`3D/`、`_schem_review/`、`v3.pdf`、`vm-preseed/`、`meta/review_feedback.md` 等本地草稿或生成输出已加入 `.gitignore`。确认要公开的硬件资料应整理进 `hardware/` 后再提交。
