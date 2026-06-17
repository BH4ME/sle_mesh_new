# V0 基线说明（1vs2 / 1vs8）

## 基线目标

1. 统一固件运行时可切 `leader/member`。
2. 在 SLE 单跳连接限制内稳定收发业务包。
3. leader 能维护基础成员表并展示到 WebUI。

## 关键实现点

- leader 采用官方 1vs8 方向的连接模型管理多连接表。
- `conn_id=0` 被视为合法连接，不能当作无效值。
- member 未被批准前进入 pending；批准后才进入 joined。
- 双 member 同时 pending/approved 与 heartbeat 已在 `v1.2.10` 现场验证。

## 典型流程

```text
member -> HELLO -> leader
leader -> pending
leader approve -> CONFIG + ACK
member -> joined + HEARTBEAT
```

## 限制

- 单跳连接仍受 SDK 物理上限约束（按 8 连接设计）。
- 当前应用层明文，`cipher_mac` 仍为预留字段。
