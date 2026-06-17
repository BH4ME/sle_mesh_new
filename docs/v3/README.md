# V3 文档入口（手机定位桥接 + SLE 位置分发）

`docs/v3/` 记录从 V2 自动组网演进到 V3 的第一阶段能力：

- 手机浏览器定位采集（WebUI）
- WS63 HTTP 定位入口
- 位置通过 `POS_REPORT` 进入 SLE 组网
- 节点位置状态回传到 `/api/nodes`

## 当前里程碑

- `v4.0.0-alpha1`：进入 V4 硬件阶段（WS63 模块 + ST7789），并把普通 member 超时失联上报闭环补齐；V4 文档入口见 `docs/v4/`。
- `v3.0.0-alpha8`：修复协议解包对齐风险、明确 relay 授权标志语义，并收紧 relay-discovery-only 广播本地处理。
- `v3.0.0-alpha7`：修复配队审批成员槽位不足时 allowlist 残留（失败回滚），并让 relay-offline 回调触发强制 rebalance（绕过 cooldown）；补充 parent-switch 返回码语义与结构契约测试说明。
- `v3.0.0-alpha6`：移除 `team_conn_track_t.addr` 冗余字段并保持 pending 地址映射链路，进一步缩减连接跟踪 RAM 占用；补充合同测试锁定结构体边界。
- `v3.0.0-alpha5`：串口模式 factory reset 明确引导（禁用按钮+提示），并精简 route hint/连接跟踪冗余字段以降低状态内存与 JSON 负担。
- `v3.0.0-alpha4`：修复 member parent timeout 轻量切换中 HELLO 发送失败导致的悬挂态，改为失败保留旧 parent 并自动重试。
- `v3.0.0-alpha3`：故障恢复优化（relay 掉线即时重平衡 + member parent 健康超时切换）与参数调优。
- `v3.0.0-alpha2`：HTTPS 局域网定位可用性增强 + HTTP 参数解析加固 + WebUI 定位入口收敛。
- `v3.0.0-alpha1`：手机定位到 SLE 位置广播链路打通。

## 版本管理

V3 起使用如下发布约束：

1. 每个版本必须包含 `versions/<version>/VERSION.md` 与 `MANIFEST.md`。
2. 每次发布必须更新 `versions/README.md` 顶部“当前版本”列表。
3. 每次发布至少执行一轮可复现验证（协议测试 / WebUI 测试 / 构建）。
