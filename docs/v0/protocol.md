# V0 协议说明（1vs2 / 1vs8）

## 分层结构

```text
Mesh Packet
  -> GROUP_DATA Payload
    -> App Packet
      -> App Body
```

## App 消息类型

- `HELLO`
- `HEARTBEAT`
- `POS_REPORT`
- `ALERT`
- `CONFIG`
- `ACK`

## 行为口径

- V0 以单跳稳定为优先，不做自动 relay 选举。
- `cipher_mac` 为预留字段，当前应用层仍为明文。
- 入网以 `HELLO -> ACK -> CONFIG` 为主流程。

## CLI

常用命令：`hello`、`hb`、`pos`、`alert`、`config`、`ack`、`members`、`state`。
