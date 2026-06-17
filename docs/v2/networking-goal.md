/goal
V2组网目标：从“V1手动relay授权 + relay主动找leaf”升级为“自动relay选举 + member自动选父 + relay失效自愈”的1vs20逻辑组网。

一、核心约束
1. 物理连接上限仍是单跳8（SDK限制），但逻辑成员目标20。
2. leader仍是唯一准入控制者（approve authority不变）。
3. 不合并到主分支，在v2分支独立迭代。

二、问题1目标（pairing window看到全部member）
1. 目标：leader在pairing window中展示尽可能完整的member列表（>=20逻辑容量），而不是只看到8个直连。
2. 结论：仅靠leader直连不可能长期同时>8；要看到更多成员必须“时间复用连接”或“经中继转发HELLO”。
3. V2方案：
- 方案A（优先）：pairing window期间启用“发现模式轮询连接”（leader循环连接候选、拉取HELLO、写入pending缓存后释放连接槽）。
- 方案B（增强）：引入“临时隐藏relay（仅转发HELLO/ROUTE_UPDATE，不转发业务）”以覆盖leader直连不到的节点。
- pending成员表与UI展示按逻辑成员容量20设计，带last_seen和来源路径。

三、问题2目标（自动relay授权与member选relay）
1. 目标：不再手动relay授权；pairing window关闭后自动产生relay集合。
2. 自动relay选举（leader侧）：
- 输入：RSSI到leader、在线稳定时长、电量、历史掉线率、当前负载。
- 输出：relay_allowed集合（限制数量与层级），下发CONFIG。
3. member自动选父（不是手工选）：
- 候选：leader或approved relay。
- 评分：RSSI、relay负载、链路稳定度、hop成本。
- 切换策略：滞回阈值 + 冷却时间，避免频繁抖动。

四、问题3目标（relay掉线自愈）
1. 触发：parent断连或heartbeat timeout。
2. 行为：member进入reselecting，重新扫描候选父节点并上报ROUTE_UPDATE。
3. leader行为：对已approved成员fast rejoin（不要求人工再approve），更新member_id->next_hop路由。
4. 组内若relay不足：自动重新选举新relay并下发授权。

五、验收标准
1. pairing window内可看到20个逻辑member（直连+轮询+临时中继路径）。
2. 关闭pairing window后自动形成relay拓扑，无需手动点relay=1。
3. 任一relay掉线后，组内leaf可在超时窗口内自动恢复上报（无人工干预）。
4. 业务路由保持next-hop定向转发，不回退为不受控泛洪。

---

当前进度（V2 Phase-1，已实现）
1. 已实现 pairing window 分时轮询连接：
- leader 在 pairing window 期间按周期轮换断开“未进入 pending/online 的连接”，释放连接槽继续扫描新候选。
- 目标是突破“同时8直连”限制，逐批发现更多 member 并写入 pending。
2. 已实现 pairing stop 自动审批：
- 关闭 pairing window 时，leader 自动审批当前 pending 列表。
- 自动 relay 授权采用首批配额策略（当前默认最多3个 relay）。
3. 已补齐 client 连接管理接口：
- 读取 active conn 列表；
- 查询 conn 绑定 member；
- 按 conn 主动断开。

---

下一版本目标（v2.0.0-alpha4）

目标定位：
- 在 alpha3 的基础上，补齐 V2 验收剩余缺口：leader 侧 relay 自动重选与授权下发。

一、要实现的能力
1. leader 侧 relay 池管理：
- leader 维护“当前 relay 集合 + 候选集合 + 最低保底数量”。
- 当 relay 数量低于阈值时，自动从在线 approved member 中补选新 relay。

2. relay 自动授权与回收：
- 对新选中的 relay 自动下发 `relay_allowed=1`（CONFIG）。
- 对长期不稳定或离线 relay 执行回收，必要时降级为普通 member。

3. relay 失效后的路由自愈闭环：
- parent 断连/超时后，leaf 自动重选父并上报 `ROUTE_UPDATE`。
- leader 收到后更新 `member_id -> next_hop`，并触发拓扑收敛检查。

4. 已审批成员 fast rejoin：
- 对已 approved member 保持免人工再次 approve；
- 重连后直接恢复组网角色与路由状态。

二、策略约束（避免抖动）
1. 选举冷却：
- relay 角色切换需要冷却窗口，避免频繁升降级。
2. 最小驻留：
- 新 relay 在最小驻留时间内不主动回收，除非硬故障。
3. 滞回阈值：
- 选举评分切换使用滞回阈值，避免边界抖动反复切换。

三、验收标准（alpha4）
1. 当任一 relay 掉线后，leader 能在窗口期内自动补齐 relay 配额（无需人工点击）。
2. 受影响 leaf 能自动重选父并恢复上报，leader 路由表收敛到新 next-hop。
3. 全流程保持“leader 唯一准入控制者”不变，且不回退为泛洪转发。
4. WebUI/日志可观察到 relay 选举、授权下发、路由收敛关键事件。

当前进度（V2 Phase-3，alpha4 进行中）
1. 已实现 leader 周期性 relay rebalance（关闭 pairing 后生效）：
- 按在线成员数计算 relay target（0/2/3 档）；
- relay 不足时自动从在线 approved 成员中按 RSSI 提升 relay，并下发 CONFIG。

2. 已实现 relay 回收与路由清理：
- offline relay 自动撤销 relay 权限；
- stale relay（超时窗口）自动下发 relay=0；
- 回收时清理依赖该 next-hop 的路由缓存，促使后续收敛。

3. 可观测性已补齐：
- 状态页新增 `Relay Target` / `Relay Online`；
- 日志新增 `relay set` / `relay rebalance` / `relay revoke` 事件。

4. 仍待补齐（下一步）：
- leader 触发后的补选闭环在复杂多跳实机场景继续校验。

---

下一版本目标（v2.0.0-alpha5）

目标定位：
- 在 alpha4 功能闭环基础上，增强 leader 侧“路由收敛可观测性”，让调试与实机定位有量化指标。

一、要实现的能力
1. 路由收敛指标：
- 统计 active/direct/relayed/unreachable/stale；
- 提供 converged 布尔态、epoch 版本、last_change/last_converged 时间戳。

2. 输出与提示：
- `GET /api/status` 暴露 `routeMetrics`；
- status 页面展示关键收敛字段；
- 指标变化写 system event，便于回放拓扑变化。

二、验收标准（alpha5）
1. status JSON 可稳定返回 `routeMetrics` 且字段完整。
2. 路由波动（不可达、陈旧、收敛恢复）可在 events 页面看到对应 system event。
3. 不改协议包结构，不影响现有 member/relay/leader 互通。

当前进度（V2 Phase-4，alpha5 收敛）
1. 已补齐 routeMetrics 字段闭环：
- `GET /api/status` 返回 `active/direct/relayed/unreachable/stale/converged/relayTarget/relayOnline/epoch/lastChangeS/lastConvergedS`。
2. status 页面已补齐路由收敛关键字段展示：
- 新增 `Route Direct`、`Route Last Change`、`Route Last Converged`；
- 与既有 `Route Converged/Active/Relayed/Unreachable/Stale/Epoch` 形成完整观测面。
3. events 页面可直接观察收敛变化：
- system event 展示方向与收敛 summary，便于回放拓扑变化；
- `last_converged` 仅在“未收敛 -> 收敛”转变时更新，避免被周期刷新覆盖。
4. 本地回归验证通过：
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 均通过。


当前进度（V2 Phase-5，alpha6 稳定性增强）
1. 已补齐时间窗口回绕安全：
- route metrics、parent switch cooldown、leader rescan、pairing rotate、relay rebalance 等周期比较统一改为回绕安全比较；
- route stale 与 relay stale/timeout 判定统一使用回绕安全路径。
2. 已提升地址派生 route id 稳定性：
- 对缺少显式 route 字节的旧广告，route id fallback 从单字节推导升级为全地址混合推导；
- 降低不同设备 route id 冲突概率，减小临时路由误识别风险。
3. 已完成验证与版本归档：
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha6` 版本记录。


当前进度（V2 Phase-6，alpha7 收敛提示闭环）
1. 已实现 leader 主动 ROUTE_UPDATE 收敛提示：
- routeMetrics 发生变化时，leader 按在线成员当前路由状态推导建议 parent 并主动下发 ROUTE_UPDATE；
- direct member 提示 parent=leader；已存在 next-hop 路由的 member 提示 parent=next_hop。
2. 可观测性已补齐：
- 新增 system event：`route hint sent=<n> fail=<n> st=<n> un=<n>`；
- 日志可观察到 route hint 聚合结果与失败项。
3. 已完成验证与版本归档：
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha7` 版本记录。


当前进度（V2 Phase-7，alpha8 提示降噪）
1. 已补齐 leader 主动 ROUTE_UPDATE 的 per-member 冷却：
- 对“同成员 + 同 parent”提示增加冷却窗口，避免 flapping 时每个 metrics 周期重复下发；
- parent 变化时可立即发送，不阻断收敛切换。
2. 可读性与行为一致性优化：
- `hint_parent_id` 下沉为循环内局部变量；
- 成功/失败路径均更新 hint 缓存时间，减少失败重试刷屏。
3. 已完成验证与版本归档：
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha8` 版本记录。


当前进度（V2 Phase-8，alpha9 指标细化）
1. 已补齐 leader 主动 ROUTE_UPDATE 的细粒度指标：
- 新增累计指标：`routeHintSentTotal`、`routeHintFailedTotal`、`routeHintCooldownSkippedTotal`；
- 新增最近活动时间：`routeHintLastActivityS`。
2. status 可观测性增强：
- `GET /api/status` 的 `routeMetrics` 已输出上述字段；
- 状态页新增 Route Hint 四项指标展示（Last Activity=0 显示 N/A）。
3. 已完成测试与版本归档：
- `examples/team_network_demo.c` 新增 JSON 字段断言；
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha9` 版本记录。


当前进度（V2 Phase-9，alpha10 多跳自愈观测增强）
1. 已补齐 leader 对 `ROUTE_UPDATE` 的累计观测：
- 新增累计指标：`routeUpdateRxTotal`（收到的 ROUTE_UPDATE 总数）；
- 新增重挂载指标：`routeReparentTotal`（next-hop 发生切换次数）；
- 新增最近重挂载时间：`routeReparentLastS`。
2. status 可观测性继续增强：
- `GET /api/status` 的 `routeMetrics` 新增上述 3 个字段；
- 状态页新增 `Route Update RX Total`、`Route Reparent Total`、`Route Reparent Last`（0 显示 N/A）。
3. 已完成测试与版本归档：
- `examples/team_network_demo.c` 新增 JSON 字段断言覆盖；
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha10` 版本记录。


当前进度（V2 Phase-10，alpha11 隐藏 relay 闭环）
1. 已落地 pairing window 的“临时隐藏 relay”模式：
- 复用 `CONFIG.reserved` 标志位下发隐藏 relay 意图（`SLE_TEAM_CONFIG_FLAG_RELAY_DISCOVERY_ONLY`）；
- member 侧新增 `relay_discovery_only` 状态并解析 CONFIG 标志；
- 当 `relay_discovery_only=1` 时，relay 仅转发 `HELLO/ROUTE_UPDATE`，不转发业务包。
2. 窗口切换自动收敛已补齐：
- leader 在 `pairing start/stop` 时主动刷新在线 relay 的 CONFIG；
- pairing 打开后进入“隐藏 relay”；pairing 关闭后自动恢复常规业务转发。
3. 测试与版本归档：
- `examples/team_network_demo.c` 新增隐藏 relay 行为断言（窗口开关标志、消息白名单转发）；
- `SLE_TEAM_NETWORK_TEST` / `SLE_TEAM_PACKET_TEST` 通过；
- 已新增 `versions/v2.0.0-alpha11` 版本记录。
4. 验收口径状态（截至 alpha11）：
- 问题1中的方案B（临时隐藏 relay）已具备实现闭环；
- V2 四条总体验收标准在能力侧均已覆盖，后续重点转向复杂多跳实机压测与参数调优。
