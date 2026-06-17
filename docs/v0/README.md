# V0（1vs2 / 1vs8 基线）

`v0` 表示“无 relay 自动化”的早期稳定阶段，目标是把单跳链路跑稳。

## 适用范围

- 1vs2 早期联调
- 1vs8 官方连接模型（leader=client/central，member=server/peripheral）
- 基础入网流程：`HELLO -> ACK -> CONFIG -> HEARTBEAT`

## 当前可参考文档

- [baseline.md](<repo-root>/docs/v0/baseline.md)
- [protocol.md](<repo-root>/docs/v0/protocol.md)

## 说明

V0 主要解决“能连上、能稳定收发、能在 WebUI 看到基础状态”，不包含 V1/V2 的 relay 管理策略。
