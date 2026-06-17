# SLE Team WebUI

这是 `sle_mesh` 的第一版 Web 控制台，参考 Meshtastic Web 和 MeshCore Web App 的产品形态，但数据模型和接口按本项目协议实现。

## 目标

- 域名上位机使用 Vite/TypeScript，可部署到 `sleweb.mecho.top`。
- WS63 板端使用 C 端 SSR，不运行前端 JS，保留 `Content-Length` 完整响应。
- 两端共享 `shared/console-pages.json` 里的产品名、入口、标签、空状态文案和配色。
- UI 通过 HTTP API 读取节点、消息和状态，不直接绑定某一种传输方式。

## 页面

- 总览：队伍状态、角色/配队控制、路由/中继指标、节点列表、消息流
- 数据包：输入十六进制包，按 `Mesh Packet -> GROUP_DATA -> App Packet` 解码
- 设置：连接方式、成员准入、板端部署和域名部署的接口说明

## 构建

```sh
npm install
npm run build
```

构建产物在：

```text
webui/dist/
```

当前产物很小，适合作为域名上位机或后续静态资源候选：

- JS gzip 约 12 KB
- CSS gzip 约 2.3 KB

注意：当前 WS63 烧录固件里的板端控制台不是直接烧录这份 Vite `dist`。为了省 RAM 并兼容 iOS/微信浏览器，板端继续使用 `xc/ws63_team_network/src/ws63_team_network_app.c` 的无 JS SSR 页面。共享内容由下面命令生成到 C 头文件：

```sh
node ../tools/gen_ws63_console_header.mjs
```

生成文件：

```text
xc/ws63_team_network/src/ws63_console_pages.h
```

## 运行

本地运行：

```sh
npm run dev
```

打开：

```text
https://localhost:5173/
```

默认不会显示假数据。进入“连接/设置”页或总览顶部可以切换：

- WiFi：填写任意一块带 HTTP API 的 WS63 地址，如 `http://192.168.43.1`
- 串口：选择 WebSerial 串口，默认 `115200`

域名上位机模式：

```text
https://sleweb.mecho.top/
https://sleweb.mecho.top/?api=http://192.168.43.1
```

如果页面和 API 同源，直接打开页面即可，不需要 `api` 参数。

注意：`https://sleweb.mecho.top` 是 HTTPS 页面。浏览器可能默认拦截它直接访问 `http://192.168.43.1` 这种私网 HTTP API。需要 WiFi 直连板端时，优先使用板端页面 `http://192.168.43.1/`；域名上位机更适合串口/WebSerial、后续 HTTPS 代理，或允许本地 HTTP 访问的浏览器环境。

部署提示（非隔离网络）：

- 外部门户 URL（例如 `sleweb.mecho.top`）应在发布后按运维策略轮换；
- 轮换后同步更新本 README、发布说明和控制台入口配置，避免旧链接长期暴露。

WiFi 入口不限定 leader。两种方式都可以落地：

- leader 开 WiFi：WebUI 能看到 leader 汇总后的全队信息。
- member 开 WiFi：WebUI 连接这个 member，能看这个 member 自身状态；如果固件把收到的配置、ACK、最近 leader 信息也缓存出来，也可以显示它看到的网络视角。

手机定位入口说明（`v4.2.2`）：

- `send location / use phone gps / start auto` 在板端 `http://192.168.43.1/pairing` 页面，不在这个 Vite 域名 WebUI 页面里。
- 域名 WebUI 的 WiFi 模式当前主要用于状态查看与控制接口调用，不提供 `POST /api/send` 和 phone GPS 采集控件。

## 当前 WS63 HTTP API

### GET /api/status

未配置时：

```json
{
  "configured": false,
  "selfLabel": "WS63-7B2C",
  "routeId": 44,
  "macReady": true,
  "macSuffix": "7B2C",
  "ssid": "SLE-7B2C",
  "transport": "ws63-softap"
}
```

已配置时：

```json
{
  "configured": true,
  "teamId": 1,
  "selfId": 1,
  "leaderId": 1,
  "macReady": true,
  "mac": "AA:BB:CC:DD:12:34",
  "macSuffix": "1234",
  "role": "leader",
  "state": "online",
  "joined": true,
  "relayAllowed": true,
  "relayEnabled": true,
  "relayTier": 0,
  "maxDownstream": 4,
  "upstreamParentId": 0,
  "upstreamParentState": "idle",
  "nextSeq": 67,
  "uptimeS": 205,
  "transport": "ws63-softap",
  "pairingEnabled": true,
  "memberFilterEnabled": false,
  "allowedMemberCount": 0,
  "allowedMembers": []
}
```

### GET /api/nodes

```json
[
  {
    "id": 2,
    "role": "member",
    "online": true,
    "batteryPercent": 88,
    "fixStatus": 1,
    "macReady": true,
    "macSuffix": "7B2C",
    "lastRssiDbm": -43,
    "relayAllowed": true,
    "relayTier": 1,
    "maxDownstream": 2,
    "parentId": 1,
    "nextHopId": 1,
    "childCount": 1,
    "lastSeq": 57,
    "lastSeenS": 205
  }
]
```

### GET /api/events

```json
[
  {
    "id": "evt-57",
    "time": "205",
    "direction": "rx",
    "type": "POS_REPORT",
    "srcId": 2,
    "dstId": 1,
    "seq": 57,
    "summary": "pos lat=39.908456 lon=116.397128 battery=88 fix=1 sat=9"
  }
]
```

`time` 是固件启动后的秒数，域名 WebUI 会显示为 `205s`。

### POST /api/send

当前 WS63 固件没有落地 HTTP `POST /api/send`；域名 WebUI 在 WiFi 模式会优先使用板端已有的 GET 控制接口，在串口模式可通过 CLI 发送测试包。`POST /api/send` 仍是后续代理/上位机接口候选。

```json
{
  "type": "position",
  "dstId": 1,
  "latitudeE6": 39908456,
  "longitudeE6": 116397128,
  "speedCms": 100,
  "headingDeg": 90,
  "batteryPercent": 88,
  "fixStatus": 1,
  "satCount": 9
}
```

后续如果上位机代理实现这个接口，建议响应返回刚产生的事件：

```json
{
  "id": "evt-tx-68",
  "time": "207",
  "direction": "tx",
  "type": "POS_REPORT",
  "srcId": 1,
  "dstId": 1,
  "seq": 68,
  "summary": "position sent to 1"
}
```

## 成员准入配置

域名 WebUI 的“成员准入”面板当前是落地功能，但写入路径限定为串口模式：

- `allow all`：leader 接收所有 member。
- `allow only 2`：leader 只接收 member 2。
- `allow add 3` / `allow del 3`：运行时增删白名单。

WiFi HTTP 当前能在 `/api/status` 里查看 `memberFilterEnabled`、`allowedMemberCount`、`allowedMembers`，成员准入写入仍走串口 CLI。角色选择、配队、member 选 leader、leave、factory reset 已同步到板端 GET 控制接口。

## 当前 WS63 HTTP 控制接口

```text
GET /api/status
GET /api/nodes
GET /api/events
GET /api/pending
GET /api/location?lat=...&lon=...&dst=255&speed=...&heading=...&battery=...&fix=...&sat=...
GET /api/config/status
GET /api/config/leader?team=1&channel=17&now=1
GET /api/config/member?leader=C7E9&team=1&channel=17&now=1
GET /api/config/apply
GET /api/config/clear
GET /api/config/reboot
GET /api/role?role=leader
GET /api/role?role=member&leader=C7E9&team=1&channel=17
GET /api/pairing?action=start|stop|approve&id=2&relay=1
GET /api/member/select?team=1&leader=C7E9&channel=17
GET /api/member/leave
GET /api/factory-reset
```

## 下一步

- 给 WS63 HTTP 增加 `POST /api/send` 和轻量配置接口。
- 把成员准入、team/channel/self/leader 配置持久化到板端存储。
- 后续用 `cipher_mac` 或 pairing key 做真实队伍认证。
