# Version Management

## 版本类型

- 仓库记录版本：记录目录整理、脚本布局、文档和硬件资料等仓库层面的变化。
- 固件版本：记录会影响 WS63 固件行为或烧录包的变化。
- 硬件版本：记录 PCB、外壳、装配资料和可制造文件的变化。

## 当前状态

- 最新仓库记录：`v4.4.129`
- 当前固件版本：`v4.4.129`
- 当前外壳版本：`hardware/enclosures/sle-pcb-enclosure/v1.1.4`

## 发布规则

- 固件行为变化时，同步更新固件源码中的版本字符串、`versions/<version>/VERSION.md` 和 `MANIFEST.md`。
- 仅目录、文档、脚本布局变化时，可以更新仓库记录版本，但不要冒充固件升级。
- 硬件资料发布时，在 `hardware/<category>/<name>/<version>/` 下放置 `README.md` 和 `MANIFEST.md`。
- 每次提交前执行与变更范围匹配的校验，例如脚本单测、WebUI 测试、仿真或远程构建。
