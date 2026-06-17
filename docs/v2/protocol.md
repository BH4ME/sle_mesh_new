# V2 协议说明（自动组网）

## 目标

在单跳 8 连接限制下，通过 relay 与路由策略实现 1vs20 逻辑成员组网。

## 分层结构

```text
Mesh Packet
  -> GROUP_DATA Payload
    -> App Packet
      -> App Body
```

## 关键消息

- `HELLO`：入网发现
- `CONFIG`：leader 下发能力/参数（含 relay 许可）
- `ACK`：关键消息确认
- `ROUTE_UPDATE`：父节点与路径更新
- `HEARTBEAT/POS_REPORT/ALERT`：业务与健康状态

## V2 路由口径

- leader 维护 `member_id -> next_hop`。
- 发包前验证 next-hop 可达性。
- 不可达时返回 `NO_ROUTE`，不做无控制泛洪。

## 安全口径

- 当前应用层仍无端到端加密；`cipher_mac` 是预留字段。
- 链路加密能力取决于 SLE 底层配对与安全配置。
