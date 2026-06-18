import {
  Battery,
  CircleDot,
  Cpu,
  ExternalLink,
  GitBranch,
  LocateFixed,
  MapPin,
  Plug,
  Radio,
  RefreshCw,
  Send,
  Settings2,
  TerminalSquare,
  Wifi,
  Wrench,
  createElement,
  type IconNode,
} from "lucide";
import {
  createTeamApi,
  getSerialDebugState,
  loadConnectionConfig,
  requestSerialPort,
  saveConnectionConfig,
  type ConnectionConfig,
} from "./api/client";
import { decodePacketHex, formatCoordinate } from "./protocol/codec";
import type {
  AllowMembersCommand,
  DeviceConfigCommand,
  DeviceConfigResult,
  DeviceConfigStatus,
  PendingMember,
  SendCommand,
  TeamEvent,
  TeamNode,
  TeamStatus,
  UnconfiguredStatus,
} from "./protocol/types";
import { formatEventTime } from "./time";
import consolePages from "../shared/console-pages.json";
import "./styles/app.css";

const hostedConsoleUrl = consolePages.hostedConsoleUrl;
const defaultDeviceApiUrl = consolePages.defaultDeviceApiUrl;

let api = createTeamApi();

interface AppState {
  status?: TeamStatus | UnconfiguredStatus;
  nodes: TeamNode[];
  pending: PendingMember[];
  events: TeamEvent[];
  deviceConfig?: DeviceConfigStatus;
  selectedTab: "overview" | "packets" | "settings";
  busy: boolean;
  error?: string;
  decodedHex: string;
  connection: ConnectionConfig;
}

const state: AppState = {
  nodes: [],
  pending: [],
  events: [],
  selectedTab: "overview",
  busy: false,
  connection: loadConnectionConfig(),
  decodedHex:
    "1A 00 11 00 00 03 00 01 00 01 02 01 01 10 00 68 F4 60 02 48 14 F0 06 78 00 5A 00 58 01 09 00",
};

const appElement = document.querySelector<HTMLDivElement>("#app");
if (!appElement) {
  throw new Error("missing #app");
}
const root = appElement;

function icon(node: IconNode, size = 18): string {
  const svg = createElement(node);
  svg.setAttribute("aria-hidden", "true");
  svg.setAttribute("width", String(size));
  svg.setAttribute("height", String(size));
  return svg.outerHTML;
}

function roleLabel(role?: string): string {
  return role === "leader" ? "Leader" : "Member";
}

function isConfiguredStatus(status?: TeamStatus | UnconfiguredStatus): status is TeamStatus {
  return status !== undefined && status.configured !== false && "teamId" in status;
}

function isStartingStatus(status?: TeamStatus | UnconfiguredStatus): boolean {
  return status !== undefined && status.configured === false && status.roleRequestPending === true;
}

function statusTeam(status?: TeamStatus | UnconfiguredStatus): string | number {
  return isConfiguredStatus(status) ? status.teamId : "--";
}

function statusSelf(status?: TeamStatus | UnconfiguredStatus): string | number {
  if (isConfiguredStatus(status)) return status.selfId;
  return status?.selfLabel || "--";
}

function stateLabel(value?: string): string {
  if (value === "online") return "Online";
  if (value === "joining") return "Joining";
  if (value === "wait_policy") return "Wait Policy";
  return "Idle";
}

function percent(value: number): string {
  return `${Math.max(0, Math.min(100, value))}%`;
}

function escapeHtml(value: string): string {
  return value
    .replace(/&/g, "&amp;")
    .replace(/</g, "&lt;")
    .replace(/>/g, "&gt;")
    .replace(/"/g, "&quot;");
}

function renderShell(): void {
  const status = state.status;
  root.innerHTML = `
    <main class="app-shell">
      <aside class="sidebar">
        <div class="brand">
          <div class="brand-mark">${icon(Radio, 24)}</div>
          <div>
            <strong>SLE Team</strong>
            <span>${escapeHtml(consolePages.product)}</span>
          </div>
        </div>
        <nav class="nav" aria-label="Primary">
          ${navButton("overview", "总览", CircleDot)}
          ${navButton("packets", "数据包", TerminalSquare)}
          ${navButton("settings", "连接/设置", Wrench)}
        </nav>
        <section class="side-status">
          <div class="label">Transport</div>
          <div class="transport">${icon(state.connection.mode === "serial" ? Plug : Wifi, 16)}${connectionLabel()}</div>
          <div class="mini-grid">
            <span>Team</span><strong>${statusTeam(status)}</strong>
            <span>Self</span><strong>${statusSelf(status)}</strong>
            <span>State</span><strong>${isConfiguredStatus(status) ? stateLabel(status.state) : isStartingStatus(status) ? "Starting" : status ? "Unconfigured" : "--"}</strong>
          </div>
        </section>
      </aside>
      <section class="workspace">
        <header class="topbar">
          <div>
            <h1>${isConfiguredStatus(status) ? `Team ${status.teamId} ${roleLabel(status.role)}` : "Team Console"}</h1>
            <p>${isConfiguredStatus(status) ? statusSubtitle(status) : status ? `device=${status.selfLabel} route=${status.routeId} ssid=${status.ssid}${isStartingStatus(status) ? " starting" : ""}` : connectionHint()}</p>
          </div>
          <button class="icon-button" data-action="refresh" title="刷新">
            ${icon(RefreshCw, 18)}
          </button>
        </header>
        ${state.error ? `<div class="error">${escapeHtml(state.error)}</div>` : ""}
        ${state.selectedTab === "overview" ? renderOverview() : ""}
        ${state.selectedTab === "packets" ? renderPackets() : ""}
        ${state.selectedTab === "settings" ? renderSettings() : ""}
      </section>
    </main>
  `;
  bindEvents();
}

function navButton(tab: AppState["selectedTab"], text: string, ico: IconNode): string {
  const active = state.selectedTab === tab ? "active" : "";
  return `<button class="nav-item ${active}" data-tab="${tab}">${icon(ico, 18)}<span>${text}</span></button>`;
}

function connectionLabel(): string {
  if (state.connection.mode === "wifi") return state.connection.apiBase || "未连接 WiFi API";
  if (state.connection.mode === "serial") return `Serial ${state.connection.serialBaud}`;
  return "未连接";
}

function connectionHint(): string {
  if (state.connection.mode === "wifi") return state.connection.apiBase ? `connecting to ${state.connection.apiBase}` : "先配置 WiFi API 地址或选择串口";
  return "通过浏览器 WebSerial 连接板子串口";
}

function statusSubtitle(status: TeamStatus): string {
  const mac = status.macSuffix ? ` mac=${status.macSuffix}` : "";
  const relay = status.relayEnabled ? " relay=on" : status.relayAllowed ? " relay=allowed" : "";
  return `self=${status.selfId} leader=${status.leaderId} seq=${status.nextSeq}${mac}${relay}`;
}

function configSummary(config?: DeviceConfigStatus): string {
  if (!config) return "not read";
  if (!config.nvValid) return `empty, self=${config.selfSuffix}`;
  if (config.nvRole === "leader") return `leader team=${config.nvTeam} channel=${config.nvChannel} self=${config.selfSuffix}`;
  return `member team=${config.nvTeam} channel=${config.nvChannel} leader=${config.nvLeaderSuffix}`;
}

function configNumberValue(primary: number | undefined, secondary: number | undefined, fallback: number): number {
  return primary ?? secondary ?? fallback;
}

function assertConfigResultOk(result: DeviceConfigResult): void {
  if (!result.ok) {
    throw new Error(`config ${result.action} failed ret=${result.ret}`);
  }
}

function renderOverview(): string {
  return `
    ${renderConnectionPanel("compact")}
    ${renderControlPanel()}
    <section class="summary-grid">
      ${metric("节点", String(state.nodes.length), Cpu)}
      ${metric("在线", String(state.nodes.filter((node) => node.online).length), Wifi)}
      ${metric("消息", String(state.events.length), TerminalSquare)}
      ${metric("入网", isConfiguredStatus(state.status) && state.status.joined ? "Yes" : "No", CircleDot)}
    </section>
    ${renderMinimalRoute()}
    ${renderLocationPanel()}
    <section class="two-column">
      <div class="panel">
        <div class="panel-head">
          <h2>节点</h2>
          <button class="text-button" data-action="refresh">刷新</button>
        </div>
        <div class="node-list">${state.nodes.map(renderNode).join("")}</div>
      </div>
      <div class="panel">
        <div class="panel-head">
          <h2>发送</h2>
        </div>
        ${renderSendForm()}
      </div>
    </section>
    <section class="panel">
      <div class="panel-head">
        <h2>消息</h2>
      </div>
      <div class="event-list">${state.events.map(renderEvent).join("")}</div>
    </section>
  `;
}

function metric(label: string, value: string, ico: IconNode): string {
  return `
    <div class="metric">
      <div>${icon(ico, 18)}<span>${label}</span></div>
      <strong>${escapeHtml(value)}</strong>
    </div>
  `;
}

function renderNode(node: TeamNode): string {
  const location = nodeLocationText(node);
  const rssiText = node.lastRssiDbm === null || node.lastRssiDbm === 127 ? "RSSI NA" : `${node.lastRssiDbm} dBm`;
  return `
    <article class="node-row">
      <div class="node-main">
        <span class="node-id">#${node.id}</span>
        <div>
          <strong>${roleLabel(node.role)}</strong>
          <span>${location}</span>
        </div>
      </div>
      <div class="node-stats">
        ${node.macSuffix ? `<span>MAC ${escapeHtml(node.macSuffix)}</span>` : ""}
        <span>${icon(Battery, 15)}${percent(node.batteryPercent)}</span>
        <span>${rssiText}</span>
        ${node.relayAllowed ? `<span>relay tier ${node.relayTier ?? 0}</span>` : ""}
        ${node.maxDownstream !== undefined ? `<span>down ${node.maxDownstream}</span>` : ""}
        ${node.parentId !== undefined ? `<span>parent ${node.parentId}</span>` : ""}
        ${node.childCount !== undefined ? `<span>children ${node.childCount}</span>` : ""}
        <span>seq ${node.lastSeq}</span>
        <span class="${node.online ? "online" : "offline"}">${node.online ? "online" : "offline"}</span>
      </div>
    </article>
  `;
}

function renderControlPanel(): string {
  const status = state.status;
  const factoryResetDisabled = state.connection.mode === "serial";
  const factoryResetHint = factoryResetDisabled
    ? `<div class="note">串口模式暂不支持 factory reset，请切换到 WiFi API 模式调用 /api/factory-reset。</div>`
    : "";
  if (!status) return "";
  if (!isConfiguredStatus(status)) {
    return `
      <section class="panel settings-panel">
        <div class="panel-head">
          <h2>角色配置</h2>
          <span class="mode-badge">${escapeHtml(status.selfLabel || "unconfigured")}</span>
        </div>
        <div class="board-summary">
          <div><span>Route</span><strong>${status.routeId}</strong></div>
          <div><span>MAC</span><strong>${escapeHtml(status.macSuffix || "--")}</strong></div>
          <div><span>SSID</span><strong>${escapeHtml(status.ssid || "--")}</strong></div>
        </div>
        <form class="role-form" data-form="role-leader">
          <label>Team<input name="teamId" type="number" min="1" max="254" value="1" /></label>
          <label>Channel<input name="channel" type="number" min="0" max="255" value="17" /></label>
          <button class="primary-button" type="submit">${icon(Settings2, 17)}设为 Leader</button>
        </form>
        <form class="role-form" data-form="role-member">
          <label>Leader MAC 后四位<input name="leaderSuffix" maxlength="4" placeholder="例如 C7E9" /></label>
          <label>Team<input name="teamId" type="number" min="1" max="254" value="1" /></label>
          <label>Channel<input name="channel" type="number" min="0" max="255" value="17" /></label>
          <button class="primary-button" type="submit">${icon(GitBranch, 17)}设为 Member</button>
        </form>
      </section>
    `;
  }
  if (status.role === "leader") {
    return `
      <section class="panel settings-panel">
        <div class="panel-head">
          <h2>Leader 配队</h2>
          <span class="mode-badge">${status.pairingEnabled ? "pairing open" : "pairing closed"}</span>
        </div>
        <div class="connection-actions control-actions">
          <button class="primary-button" type="button" data-action="pairing-start">${icon(Radio, 17)}开始配队</button>
          <button class="text-button" type="button" data-action="pairing-stop">停止并自动批准</button>
          <button class="text-button" type="button" data-action="factory-reset" ${factoryResetDisabled ? "disabled" : ""}>Factory reset</button>
        </div>
        ${factoryResetHint}
        <div class="pending-list">
          ${state.pending.length === 0 ? `<div class="empty-state">暂无 pending member</div>` : state.pending.map(renderPendingMember).join("")}
        </div>
      </section>
    `;
  }
  return `
    <section class="panel settings-panel">
      <div class="panel-head">
        <h2>Member 连接</h2>
        <span class="mode-badge">${status.joined ? "joined" : "not joined"}</span>
      </div>
      <div class="board-summary">
        <div><span>Parent</span><strong>${status.upstreamParentId ?? 0}</strong></div>
        <div><span>Parent State</span><strong>${status.upstreamParentState ?? "--"}</strong></div>
        <div><span>Relay</span><strong>${status.relayEnabled ? "enabled" : status.relayAllowed ? "allowed" : "off"}</strong></div>
      </div>
      <form class="role-form" data-form="member-select">
        <label>Leader MAC 后四位<input name="leaderSuffix" maxlength="4" placeholder="例如 C7E9" /></label>
        <label>Team<input name="teamId" type="number" min="1" max="254" value="${status.teamId}" /></label>
        <label>Channel<input name="channel" type="number" min="0" max="255" value="17" /></label>
        <button class="primary-button" type="submit">${icon(GitBranch, 17)}选择 Leader</button>
        <button class="text-button" type="button" data-action="member-leave">Leave</button>
        <button class="text-button" type="button" data-action="factory-reset" ${factoryResetDisabled ? "disabled" : ""}>Factory reset</button>
      </form>
      ${factoryResetHint}
    </section>
  `;
}

function renderPendingMember(member: PendingMember): string {
  return `
    <article class="pending-row">
      <div>
        <strong>#${member.id} ${escapeHtml(member.macSuffix || "")}</strong>
        <span>${roleLabel(member.role)} battery=${member.batteryPercent}% seen=${member.lastSeenS}s</span>
      </div>
      <div class="connection-actions">
        <button class="text-button" type="button" data-action="pending-approve-relay" data-id="${member.id}">approve relay</button>
        <button class="text-button" type="button" data-action="pending-approve-norelay" data-id="${member.id}">approve no-relay</button>
      </div>
    </article>
  `;
}

function renderMinimalRoute(): string {
  const status = state.status;
  if (!isConfiguredStatus(status)) return "";
  return `
    <section class="panel route-panel">
      <div class="panel-head">
        <h2>路由/中继</h2>
        <span class="mode-badge">minimal</span>
      </div>
      <div class="route-grid">
        <div><span>Relay</span><strong>${status.relayEnabled ? "enabled" : status.relayAllowed ? "allowed" : "off"}</strong></div>
        <div><span>Tier</span><strong>${status.relayTier ?? 0}</strong></div>
        <div><span>Max Downstream</span><strong>${status.maxDownstream ?? 0}</strong></div>
        <div><span>Parent</span><strong>${status.upstreamParentId ?? 0}</strong></div>
        <div><span>Parent State</span><strong>${status.upstreamParentState ?? "--"}</strong></div>
      </div>
    </section>
  `;
}

function nodeHasPosition(node: TeamNode): boolean {
  return (
    node.latitudeE6 !== undefined &&
    node.longitudeE6 !== undefined &&
    (node.positionValid === true || node.fixStatus !== 0)
  );
}

function nodeLocationText(node: TeamNode): string {
  if (!nodeHasPosition(node) || node.latitudeE6 === undefined || node.longitudeE6 === undefined) {
    return "no position";
  }
  return `${formatCoordinate(node.latitudeE6)}, ${formatCoordinate(node.longitudeE6)}`;
}

function nodeMapUrl(node: TeamNode): string {
  if (!nodeHasPosition(node) || node.latitudeE6 === undefined || node.longitudeE6 === undefined) {
    return "#";
  }
  const lat = (node.latitudeE6 / 1_000_000).toFixed(6);
  const lon = (node.longitudeE6 / 1_000_000).toFixed(6);
  return `https://www.google.com/maps/search/?api=1&query=${lat},${lon}`;
}

function renderLocationPanel(): string {
  const positioned = state.nodes.filter(nodeHasPosition);
  const shareDisabled = isConfiguredStatus(state.status) ? "" : "disabled";
  return `
    <section class="panel location-panel">
      <div class="panel-head">
        <h2>${icon(MapPin, 17)} Location</h2>
        <div class="location-actions">
          <button class="text-button" type="button" data-action="share-phone-location" ${shareDisabled} title="Share this browser GPS fix">
            ${icon(LocateFixed, 17)}Share phone GPS
          </button>
        </div>
      </div>
      ${
        positioned.length === 0
          ? `<div class="empty-state location-empty">No node positions yet</div>`
          : `<div class="location-grid">${positioned.map(renderPositionCard).join("")}</div>`
      }
    </section>
  `;
}

function renderPositionCard(node: TeamNode): string {
  const stale = node.online ? "Live fix" : "Last known";
  const speed = node.speedCms !== undefined ? `${node.speedCms} cm/s` : "--";
  const heading = node.headingDeg !== undefined ? `${node.headingDeg} deg` : "--";
  const sat = node.satCount !== undefined ? String(node.satCount) : "--";
  return `
    <article class="position-card ${node.online ? "" : "stale"}">
      <div class="position-top">
        <div>
          <strong>#${node.id} ${roleLabel(node.role)}</strong>
          <span>${stale}</span>
        </div>
        <a class="map-link" href="${escapeHtml(nodeMapUrl(node))}" target="_blank" rel="noreferrer" title="Open map">
          ${icon(ExternalLink, 16)}
        </a>
      </div>
      <div class="position-value">${escapeHtml(nodeLocationText(node))}</div>
      <div class="position-meta">
        <span>speed ${speed}</span>
        <span>heading ${heading}</span>
        <span>sat ${sat}</span>
      </div>
    </article>
  `;
}

function renderSendForm(): string {
  if (state.connection.mode === "wifi") {
    return `
      <div class="send-form">
        <div class="note">WiFi 模式暂不提供 /api/send；请切到串口模式发送测试包。</div>
      </div>
    `;
  }
  return `
    <form class="send-form" data-form="send">
      <label>类型
        <select name="type">
          <option value="heartbeat">HEARTBEAT</option>
          <option value="position">POS_REPORT</option>
          <option value="alert">ALERT</option>
          <option value="config">CONFIG</option>
        </select>
      </label>
      <div class="form-grid">
        <label>Dst<input name="dstId" type="number" min="1" max="255" value="1" /></label>
        <label>电量<input name="batteryPercent" type="number" min="0" max="100" value="88" /></label>
        <label>RSSI<input name="rssiDbm" type="number" min="-128" max="127" value="-43" /></label>
        <label>Fix<input name="fixStatus" type="number" min="0" max="255" value="1" /></label>
        <label>LatE6<input name="latitudeE6" type="number" value="39908456" /></label>
        <label>LonE6<input name="longitudeE6" type="number" value="116397128" /></label>
        <label>速度<input name="speedCms" type="number" min="0" value="100" /></label>
        <label>航向<input name="headingDeg" type="number" min="0" value="90" /></label>
        <label>Sat<input name="satCount" type="number" min="0" max="255" value="8" /></label>
      </div>
      <button class="primary-button" type="submit">${icon(Send, 17)}发送</button>
    </form>
  `;
}

function renderEvent(event: TeamEvent): string {
  const labels: Record<TeamEvent["direction"], string> = {
    rx: "SLE RX",
    tx: "SLE TX",
    fail: "失败",
    cli: "串口",
    state: "状态",
    system: "系统",
  };
  return `
    <article class="event-row">
      <div class="event-type ${event.direction}">${labels[event.direction]}</div>
      <div>
        <strong>${event.type}</strong>
        <span>${escapeHtml(event.summary)}</span>
      </div>
      <time>${formatEventTime(event.time)}</time>
    </article>
  `;
}

function renderPackets(): string {
  const decoded = decodePacketHex(state.decodedHex);
  return `
    <section class="panel packet-workbench">
      <div class="panel-head">
        <h2>包解析</h2>
      </div>
      <textarea class="hex-input" data-hex-input spellcheck="false">${escapeHtml(state.decodedHex)}</textarea>
      <div class="decode-result">
        ${
          decoded.ok
            ? `
              <div class="decode-grid">
                <span>Mesh</span><strong>v${decoded.mesh?.version} payload=${decoded.mesh?.payloadType} route=${decoded.mesh?.routeType}</strong>
                <span>Channel</span><strong>${decoded.mesh?.channelHash ?? "--"}</strong>
                <span>App</span><strong>${decoded.app?.type ?? "--"} seq=${decoded.app?.seq ?? "--"} ${decoded.app?.srcId ?? "--"}→${decoded.app?.dstId ?? "--"}</strong>
                <span>Body</span><pre>${escapeHtml(JSON.stringify(decoded.app?.body ?? {}, null, 2))}</pre>
              </div>
            `
            : `<div class="error">${escapeHtml(decoded.error ?? "decode failed")}</div>`
        }
      </div>
    </section>
  `;
}

function renderSettings(): string {
  return `
    ${renderConnectionPanel("full")}
    ${renderBulkConfigPanel()}
    ${renderAllowPanel()}
    <section class="panel settings-panel">
      <div class="panel-head">
        <h2>部署</h2>
      </div>
      <div class="deploy-grid">
        <div>
          <strong>WS63 板端</strong>
          <span>当前烧录固件使用 C 端 SSR 页面，不烧 Vite dist；板端 HTTP 服务直接输出轻量 HTML。</span>
          <code>/ /nodes /events /pairing /api/status</code>
        </div>
        <div>
          <strong>域名上位机</strong>
          <span>部署在 sleweb.mecho.top，串口连接可直接使用；WiFi API 直连私网设备时受浏览器跨域与私网访问策略影响。</span>
          <code>${hostedConsoleUrl}/?api=${defaultDeviceApiUrl}</code>
        </div>
      </div>
    </section>
    <section class="panel">
      <div class="panel-head">
        <h2>接口草案</h2>
      </div>
      <pre class="api-spec">GET  /api/status
GET  /api/nodes
GET  /api/events
GET  /api/pending
GET  /api/config/status
GET  /api/config/leader?team=1&channel=17&now=1
GET  /api/config/member?leader=C7E9&team=1&channel=17&now=1
GET  /api/config/apply
GET  /api/config/clear
GET  /api/config/reboot
GET  /api/role?role=leader
GET  /api/role?role=member&leader=C7E9&team=1&channel=17
GET  /api/pairing?action=start|stop|approve&id=2&relay=1
GET  /api/member/select?team=1&leader=C7E9&channel=17
GET  /api/member/leave
GET  /api/factory-reset</pre>
    </section>
  `;
}

function renderAllowPanel(): string {
  const status = state.status;
  const configured = isConfiguredStatus(status) ? status : undefined;
  const allowMode = configured?.memberFilterEnabled ? "only" : "all";
  const allowMembers = configured?.allowedMembers?.length
    ? configured.allowedMembers.join(" ")
    : configured?.allowedMemberCount
      ? `${configured.allowedMemberCount} 个成员，串口刷新可展开 ID`
      : "";
  const serialOnly = state.connection.mode === "serial" ? "" : `<div class="note">成员准入写入目前走板子串口 CLI；WiFi HTTP 页面先做状态查看。</div>`;
  return `
    <section class="panel settings-panel">
      <div class="panel-head">
        <h2>成员准入</h2>
        <span class="mode-badge">${allowMode === "all" ? "allow all" : "allow only"}</span>
      </div>
      <div class="allow-summary">
        <div><span>Team</span><strong>${configured?.teamId ?? "--"}</strong></div>
        <div><span>Leader</span><strong>${configured?.leaderId ?? "--"}</strong></div>
        <div><span>Self</span><strong>${configured?.selfId ?? "--"}</strong></div>
        <div><span>Allowed</span><strong>${escapeHtml(allowMembers || "all")}</strong></div>
      </div>
      <form class="allow-form" data-form="allow">
        <div class="segmented" role="tablist" aria-label="Member allow mode">
          <label class="segment"><input type="radio" name="allowMode" value="all" ${allowMode === "all" ? "checked" : ""} /><span>全部成员</span></label>
          <label class="segment"><input type="radio" name="allowMode" value="only" ${allowMode === "only" ? "checked" : ""} /><span>只允许列表</span></label>
        </div>
        <label>Member ID 列表
          <input name="memberIds" type="text" inputmode="numeric" placeholder="例如 2 或 2 3 4" value="${escapeHtml(configured?.allowedMembers?.join(" ") ?? "")}" />
        </label>
        <div class="connection-actions">
          <button class="primary-button" type="submit">${icon(Plug, 17)}应用准入</button>
          <button class="text-button" type="button" data-action="allow-add">添加</button>
          <button class="text-button" type="button" data-action="allow-del">删除</button>
        </div>
        ${serialOnly}
      </form>
    </section>
  `;
}

function renderBulkConfigPanel(): string {
  const config = state.deviceConfig;
  const serial = getSerialDebugState();
  return `
    <section class="panel settings-panel bulk-config-panel">
      <div class="panel-head">
        <h2>One-click node config</h2>
        <span class="mode-badge">${escapeHtml(configSummary(config))}</span>
      </div>
      <div class="config-summary">
        <div><span>NV Role</span><strong>${config?.nvValid ? config.nvRole : "empty"}</strong></div>
        <div><span>NV Team</span><strong>${config?.nvValid ? config.nvTeam : "--"}</strong></div>
        <div><span>NV Channel</span><strong>${config?.nvValid ? config.nvChannel : "--"}</strong></div>
        <div><span>Leader Suffix</span><strong>${config?.nvValid ? config.nvLeaderSuffix : "--"}</strong></div>
        <div><span>Runtime</span><strong>${config?.runtimeConfigured ? config.runtimeRole : config?.roleRequestPending ? `starting ${config.roleRequestRole}` : "unconfigured"}</strong></div>
        <div><span>Self Suffix</span><strong>${config?.selfSuffix ?? "--"}</strong></div>
      </div>
      <form class="bulk-config-form" data-form="bulk-config">
        <div class="segmented" role="tablist" aria-label="Bulk node role">
          <label class="segment"><input type="radio" name="role" value="leader" checked /><span>Leader</span></label>
          <label class="segment"><input type="radio" name="role" value="member" /><span>Member</span></label>
        </div>
        <div class="bulk-config-fields">
          <label>Team ID<input name="teamId" type="number" min="1" max="254" value="${configNumberValue(config?.nvTeam, config?.runtimeTeam, 1)}" /></label>
          <label>Channel<input name="channel" type="number" min="0" max="255" value="${configNumberValue(config?.nvChannel, config?.runtimeChannel, 17)}" /></label>
          <label>Leader suffix<input name="leaderSuffix" maxlength="4" placeholder="C7E9" value="${config?.nvRole === "member" ? config.nvLeaderSuffix : ""}" /></label>
        </div>
        <label class="check-row">
          <input name="applyNow" type="checkbox" checked />
          <span>Apply immediately after saving. If unchecked, save to flash and use Apply/Reboot later.</span>
        </label>
        <div class="connection-actions">
          <button class="primary-button" type="submit">${icon(Settings2, 17)}Write config</button>
          <button class="text-button" type="button" data-action="config-read">Read back</button>
          <button class="text-button" type="button" data-action="config-apply">Apply saved</button>
          <button class="text-button" type="button" data-action="config-clear">Clear saved</button>
          <button class="text-button" type="button" data-action="config-reboot">Reboot board</button>
        </div>
        <div class="note">
          Serial mode uses <code>cfg status</code>, <code>cfg leader/member</code>, <code>cfg apply</code>, <code>cfg clear</code> and shows returned logs below.
        </div>
      </form>
      <div class="serial-log">
        <div class="panel-head small-head">
          <h2>Serial log</h2>
          <span class="mode-badge">${serial.connected ? "connected" : "not connected"}</span>
        </div>
        <pre>${escapeHtml(serial.lines.slice(-36).join("\n") || "No serial lines yet. Click Select serial, then Read back.")}</pre>
      </div>
    </section>
  `;
}

function renderConnectionPanel(layout: "compact" | "full"): string {
  const title = layout === "compact" ? "当前连接" : "连接配置";
  return `
    <section class="panel settings-panel connection-panel">
      <div class="panel-head">
        <h2>${title}</h2>
        <span class="mode-badge">${connectionLabel()}</span>
      </div>
      <form class="connection-form ${layout === "compact" ? "compact" : ""}" data-form="connection">
        <div class="segmented" role="tablist" aria-label="Connection mode">
          ${connectionOption("wifi", "WiFi API")}
          ${connectionOption("serial", "串口")}
        </div>
        <div class="connection-fields">
          <label>WS63 HTTP API 地址
            <input name="apiBase" type="url" placeholder="${defaultDeviceApiUrl} 或 http://member-ip" value="${escapeHtml(state.connection.apiBase)}" />
          </label>
          <label>串口波特率
            <input name="serialBaud" type="number" min="9600" max="921600" value="${state.connection.serialBaud}" />
          </label>
        </div>
        <div class="connection-actions">
          <button class="primary-button" type="submit">${icon(Plug, 17)}保存并重连</button>
          <button class="text-button" type="button" data-action="serial-connect">选择串口</button>
        </div>
        <div class="note">
          当前是 <strong>${connectionLabel()}</strong>。WiFi 可以连接任意一块带 HTTP API 的 WS63，leader 或 member 都可以；串口是浏览器直接连接一块板子的 UART CLI。域名上位机地址是 <strong>${hostedConsoleUrl}</strong>。
        </div>
      </form>
    </section>
  `;
}

function connectionOption(mode: ConnectionConfig["mode"], label: string): string {
  const checked = state.connection.mode === mode ? "checked" : "";
  return `<label class="segment"><input type="radio" name="mode" value="${mode}" ${checked} /><span>${label}</span></label>`;
}

function readNumber(form: FormData, key: string): number | undefined {
  const value = form.get(key);
  if (typeof value !== "string" || value.trim() === "") return undefined;
  return Number(value);
}

function readChecked(form: FormData, key: string): boolean {
  return form.get(key) !== null;
}

function metersPerSecondToCms(value: number | null): number {
  if (value === null || !Number.isFinite(value)) return 0;
  return Math.max(0, Math.min(65535, Math.round(value * 100)));
}

function headingToDeg(value: number | null): number {
  if (value === null || !Number.isFinite(value)) return 0;
  return Math.round(((value % 360) + 360) % 360);
}

function phonePosition(position: GeolocationPosition): SendCommand {
  const status = isConfiguredStatus(state.status) ? state.status : undefined;
  const coords = position.coords;
  return {
    type: "position",
    dstId: status?.role === "member" ? status.leaderId : 255,
    latitudeE6: Math.round(coords.latitude * 1_000_000),
    longitudeE6: Math.round(coords.longitude * 1_000_000),
    speedCms: metersPerSecondToCms(coords.speed),
    headingDeg: headingToDeg(coords.heading),
    batteryPercent: 100,
    fixStatus: 1,
    satCount: 0,
  };
}

async function sharePhoneLocation(): Promise<void> {
  if (!isConfiguredStatus(state.status)) {
    state.error = "configure this node before sharing location";
    renderShell();
    return;
  }
  if (!navigator.geolocation) {
    state.error = "browser geolocation is unavailable";
    renderShell();
    return;
  }
  try {
    const position = await new Promise<GeolocationPosition>((resolve, reject) => {
      navigator.geolocation.getCurrentPosition(resolve, reject, {
        enableHighAccuracy: true,
        timeout: 10000,
        maximumAge: 5000,
      });
    });
    const eventResult = await api.send(phonePosition(position));
    state.events.unshift(eventResult);
    state.error = undefined;
    await refresh();
  } catch (error) {
    state.error = error instanceof Error ? error.message : "phone location failed";
    renderShell();
  }
}

function readMemberIds(raw: string): number[] {
  return raw
    .split(/[\s,，]+/)
    .map((part) => part.trim())
    .filter(Boolean)
    .map(Number)
    .filter((value) => Number.isInteger(value) && value >= 1 && value <= 254);
}

function readLeaderSuffix(form: FormData): string {
  const suffix = String(form.get("leaderSuffix") ?? "").trim().toUpperCase();
  if (!/^[0-9A-F]{4}$/.test(suffix)) {
    throw new Error("Leader MAC 后四位需要 4 位十六进制，例如 C7E9");
  }
  return suffix;
}

async function runAction(action: () => Promise<void>, failure = "operation failed"): Promise<void> {
  try {
    await action();
    state.error = undefined;
    await refresh();
  } catch (error) {
    state.error = error instanceof Error ? error.message : failure;
    renderShell();
  }
}

async function applyAllow(command: AllowMembersCommand): Promise<void> {
  try {
    const eventResult = await api.configureAllow(command);
    state.events.unshift(eventResult);
    state.error = undefined;
    await refresh();
  } catch (error) {
    state.error = error instanceof Error ? error.message : "allow config failed";
    renderShell();
  }
}

async function readDeviceConfig(): Promise<void> {
  state.deviceConfig = await api.getDeviceConfig();
}

async function runConfigAction(action: () => Promise<void>, failure = "config operation failed"): Promise<void> {
  try {
    await action();
    state.error = undefined;
    await refresh();
  } catch (error) {
    state.error = error instanceof Error ? error.message : failure;
    renderShell();
  }
}

async function refresh(): Promise<void> {
  if (state.busy) {
    return;
  }
  if (state.connection.mode === "wifi" && state.connection.apiBase === "") {
    state.status = undefined;
    state.nodes = [];
    state.pending = [];
    state.events = [];
    state.error = undefined;
    renderShell();
    return;
  }
  state.busy = true;
  state.error = undefined;
  renderShell();
  try {
    state.status = await api.getStatus();
    state.nodes = await api.getNodes();
    state.events = await api.getEvents();
    try {
      await readDeviceConfig();
    } catch {
      state.deviceConfig = undefined;
    }
    if (isConfiguredStatus(state.status) && state.status.role === "leader") {
      try {
        state.pending = await api.getPending();
      } catch {
        state.pending = [];
      }
    } else {
      state.pending = [];
    }
  } catch (error) {
    state.error = error instanceof Error ? error.message : "refresh failed";
  } finally {
    state.busy = false;
    renderShell();
  }
}

function bindEvents(): void {
  document.querySelectorAll<HTMLButtonElement>("[data-tab]").forEach((button) => {
    button.addEventListener("click", () => {
      state.selectedTab = button.dataset.tab as AppState["selectedTab"];
      renderShell();
    });
  });
  document.querySelectorAll<HTMLButtonElement>("[data-action='refresh']").forEach((button) => {
    button.addEventListener("click", () => void refresh());
  });
  document.querySelector<HTMLTextAreaElement>("[data-hex-input]")?.addEventListener("input", (event) => {
    state.decodedHex = (event.target as HTMLTextAreaElement).value;
    renderShell();
  });
  document.querySelector<HTMLFormElement>("[data-form='send']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    const command: SendCommand = {
      type: String(form.get("type")) as SendCommand["type"],
      dstId: readNumber(form, "dstId") ?? 1,
      batteryPercent: readNumber(form, "batteryPercent"),
      rssiDbm: readNumber(form, "rssiDbm"),
      fixStatus: readNumber(form, "fixStatus"),
      latitudeE6: readNumber(form, "latitudeE6"),
      longitudeE6: readNumber(form, "longitudeE6"),
      speedCms: readNumber(form, "speedCms"),
      headingDeg: readNumber(form, "headingDeg"),
      satCount: readNumber(form, "satCount"),
    };
    void api
      .send(command)
      .then((eventResult) => {
        state.events.unshift(eventResult);
        return refresh();
      })
      .catch((error) => {
        state.error = error instanceof Error ? error.message : "send failed";
        renderShell();
      });
  });
  document.querySelector<HTMLFormElement>("[data-form='role-leader']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    void runAction(
      () =>
        api.configureRole({
          role: "leader",
          teamId: readNumber(form, "teamId") ?? 1,
          channel: readNumber(form, "channel") ?? 17,
        }),
      "set leader failed",
    );
  });
  document.querySelector<HTMLFormElement>("[data-form='role-member']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    void runAction(
      () =>
        api.configureRole({
          role: "member",
          leaderSuffix: readLeaderSuffix(form),
          teamId: readNumber(form, "teamId") ?? 1,
          channel: readNumber(form, "channel") ?? 17,
        }),
      "set member failed",
    );
  });
  document.querySelector<HTMLFormElement>("[data-form='member-select']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    void runAction(
      () =>
        api.selectMemberLeader({
          leaderSuffix: readLeaderSuffix(form),
          teamId: readNumber(form, "teamId") ?? 1,
          channel: readNumber(form, "channel") ?? 17,
        }),
      "select leader failed",
    );
  });
  document.querySelector<HTMLButtonElement>("[data-action='pairing-start']")?.addEventListener("click", () => {
    void runAction(() => api.configurePairing({ action: "start" }), "pairing start failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='pairing-stop']")?.addEventListener("click", () => {
    void runAction(() => api.configurePairing({ action: "stop" }), "pairing stop failed");
  });
  document.querySelectorAll<HTMLButtonElement>("[data-action='pending-approve-relay'], [data-action='pending-approve-norelay']").forEach((button) => {
    button.addEventListener("click", () => {
      const id = Number(button.dataset.id);
      if (!Number.isInteger(id)) return;
      void runAction(
        () =>
          api.configurePairing({
            action: "approve",
            id,
            relay: button.dataset.action === "pending-approve-relay",
          }),
        "approve pending failed",
      );
    });
  });
  document.querySelector<HTMLButtonElement>("[data-action='member-leave']")?.addEventListener("click", () => {
    void runAction(() => api.leaveMember(), "leave failed");
  });
  document.querySelectorAll<HTMLButtonElement>("[data-action='factory-reset']").forEach((button) => {
    button.addEventListener("click", () => {
      void runAction(() => api.factoryReset(), "factory reset failed");
    });
  });
  document.querySelector<HTMLFormElement>("[data-form='connection']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    const nextConfig: ConnectionConfig = {
      mode: String(form.get("mode")) as ConnectionConfig["mode"],
      apiBase: String(form.get("apiBase") ?? "").trim().replace(/\/$/, ""),
      serialBaud: readNumber(form, "serialBaud") ?? 115200,
    };
    if (nextConfig.mode === "wifi" && nextConfig.apiBase === "") {
      state.error = `WiFi 模式需要填写 WS63 HTTP API 地址，例如 ${defaultDeviceApiUrl}`;
      renderShell();
      return;
    }
    saveConnectionConfig(nextConfig);
    state.connection = nextConfig;
    api = createTeamApi();
    state.error = undefined;
    void refresh();
  });
  document.querySelector<HTMLFormElement>("[data-form='allow']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    const mode = String(form.get("allowMode")) === "only" ? "only" : "all";
    const memberIds = readMemberIds(String(form.get("memberIds") ?? ""));
    void applyAllow({ mode, memberIds });
  });
  document.querySelector<HTMLButtonElement>("[data-action='allow-add']")?.addEventListener("click", () => {
    const input = document.querySelector<HTMLInputElement>("input[name='memberIds']");
    const memberIds = readMemberIds(input?.value ?? "");
    void applyAllow({ mode: "add", memberIds: memberIds.slice(0, 1) });
  });
  document.querySelector<HTMLButtonElement>("[data-action='allow-del']")?.addEventListener("click", () => {
    const input = document.querySelector<HTMLInputElement>("input[name='memberIds']");
    const memberIds = readMemberIds(input?.value ?? "");
    void applyAllow({ mode: "del", memberIds: memberIds.slice(0, 1) });
  });
  document.querySelector<HTMLFormElement>("[data-form='bulk-config']")?.addEventListener("submit", (event) => {
    event.preventDefault();
    const formElement = event.currentTarget;
    if (!(formElement instanceof HTMLFormElement)) return;
    const form = new FormData(formElement);
    const role = String(form.get("role")) === "member" ? "member" : "leader";
    const base = {
      teamId: readNumber(form, "teamId") ?? 1,
      channel: readNumber(form, "channel") ?? 17,
      applyNow: readChecked(form, "applyNow"),
    };
    const command: DeviceConfigCommand =
      role === "leader" ? { role, ...base } : { role, leaderSuffix: readLeaderSuffix(form), ...base };
    void runConfigAction(async () => {
      const result = await api.configureDevice(command);
      assertConfigResultOk(result);
      state.deviceConfig = result.config;
    }, "write config failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='config-read']")?.addEventListener("click", () => {
    void runConfigAction(async () => {
      await readDeviceConfig();
    }, "read config failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='config-apply']")?.addEventListener("click", () => {
    void runConfigAction(async () => {
      const result = await api.applyDeviceConfig();
      assertConfigResultOk(result);
      state.deviceConfig = result.config;
    }, "apply config failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='config-clear']")?.addEventListener("click", () => {
    void runConfigAction(async () => {
      const result = await api.clearDeviceConfig();
      assertConfigResultOk(result);
      state.deviceConfig = result.config;
    }, "clear config failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='config-reboot']")?.addEventListener("click", () => {
    void runConfigAction(async () => {
      const result = await api.rebootDevice();
      assertConfigResultOk(result);
      state.deviceConfig = result.config;
    }, "reboot failed");
  });
  document.querySelector<HTMLButtonElement>("[data-action='serial-connect']")?.addEventListener("click", () => {
    void connectSerialPlaceholder();
  });
  document.querySelector<HTMLButtonElement>("[data-action='share-phone-location']")?.addEventListener("click", () => {
    void sharePhoneLocation();
  });
}

async function connectSerialPlaceholder(): Promise<void> {
  const serialNavigator = navigator as Navigator & {
    serial?: {
      requestPort(options?: unknown): Promise<unknown>;
    };
  };
  if (!serialNavigator.serial) {
    state.error = "当前浏览器不支持 WebSerial。Chrome/Edge 可用，Safari 暂不支持。";
    renderShell();
    return;
  }
  try {
    await requestSerialPort(state.connection.serialBaud);
    state.error = undefined;
    state.connection = { ...state.connection, mode: "serial" };
    saveConnectionConfig(state.connection);
    api = createTeamApi();
    await refresh();
    return;
  } catch (error) {
    state.error = error instanceof Error ? error.message : "serial selection cancelled";
  }
  renderShell();
}

renderShell();
void refresh();
window.setInterval(() => {
  if (state.connection.mode === "wifi") {
    void refresh();
  }
}, 5000);
window.setInterval(() => {
  if (state.connection.mode === "serial") {
    void refresh();
  }
}, 15000);
