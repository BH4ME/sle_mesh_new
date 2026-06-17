# V1 组网流程（手动 relay）

## 目标

在 1vs8 单跳物理限制下，先实现可控的一对多逻辑组网，并引入 relay 但保持人工可控。

## 角色职责

- leader：配对窗口控制、成员 approve、relay 权限授予。
- relay member：被授权后可连接下游 leaf 并转发。
- leaf member：普通业务节点，不承担中继。

## 操作流程

1. leader 打开 pairing window。
2. member 发 `HELLO` 进入 pending。
3. leader 在 `/pairing` 手动 approve。
4. leader 可对指定 member 显式设 `relay=1`。
5. relay 主动吸纳 leaf，形成分层链路。

## 路由规则

- 发包优先走 `next_hop` 定向转发。
- next-hop 不可达时直接 `NO_ROUTE`，避免不受控泛洪。

## 与 V2 的区别

- V1：relay 授权与拓扑形成是人工主导。
- V2：relay 选举、选父、掉线自愈转向自动策略。
