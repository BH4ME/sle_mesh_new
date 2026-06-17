# V1 协议说明（手动 relay）

## 与 V0 的差异

- 增加 relay 相关能力，但授权仍由 leader 手动控制。
- 路由由 `member_id -> next_hop` 记录驱动定向转发。

## 主要消息

- 保留 V0 的 `HELLO/HEARTBEAT/POS_REPORT/ALERT/CONFIG/ACK`
- 增加 `ROUTE_UPDATE`（用于上报父节点与路径变化）

## 路由策略

- 有 next-hop 时只走 next-hop。
- next-hop 不可达时直接 `NO_ROUTE`，避免 fallback 泛洪。
