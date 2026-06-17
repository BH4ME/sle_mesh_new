# V1（手动 relay 批准）

`v1` 表示“leader 手动批准 relay + relay 主动找 leaf”的组网阶段。

## 核心模型

- leader 是唯一准入与授权方。
- member 先 `approve/join`，再按 leader 显式授权启用 relay。
- relay 主动扫描并连接 leaf，leaf 不手工选择 relay。
- 路由使用 `member_id -> next_hop` 学习与定向转发。

## 文档

- [networking.md](<repo-root>/docs/v1/networking.md)
- [protocol.md](<repo-root>/docs/v1/protocol.md)
