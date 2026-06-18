import type {
  AllowMembersCommand,
  DeviceConfigCommand,
  DeviceConfigResult,
  DeviceConfigStatus,
  MemberSelectCommand,
  PairingCommand,
  PendingMember,
  RoleCommand,
  SendCommand,
  TeamEvent,
  TeamNode,
  TeamStatus,
  UnconfiguredStatus,
} from "../protocol/types";
import { fetchAction, fetchJson } from "./http";

export interface TeamApi {
  getStatus(): Promise<TeamStatus | UnconfiguredStatus>;
  getNodes(): Promise<TeamNode[]>;
  getEvents(): Promise<TeamEvent[]>;
  getPending(): Promise<PendingMember[]>;
  configureRole(command: RoleCommand): Promise<void>;
  configurePairing(command: PairingCommand): Promise<void>;
  selectMemberLeader(command: MemberSelectCommand): Promise<void>;
  leaveMember(): Promise<void>;
  factoryReset(): Promise<void>;
  send(command: SendCommand): Promise<TeamEvent>;
  configureAllow(command: AllowMembersCommand): Promise<TeamEvent>;
  getDeviceConfig(): Promise<DeviceConfigStatus>;
  configureDevice(command: DeviceConfigCommand): Promise<DeviceConfigResult>;
  applyDeviceConfig(): Promise<DeviceConfigResult>;
  clearDeviceConfig(): Promise<DeviceConfigResult>;
  rebootDevice(): Promise<DeviceConfigResult>;
}

export type ConnectionMode = "wifi" | "serial";

export interface ConnectionConfig {
  mode: ConnectionMode;
  apiBase: string;
  serialBaud: number;
}

const defaultConfig: ConnectionConfig = {
  mode: "wifi",
  apiBase: "",
  serialBaud: 115200,
};

const configKey = "sle-team-connection";

export function loadConnectionConfig(): ConnectionConfig {
  const url = new URL(window.location.href);
  const api = url.searchParams.get("api");
  if (api !== null) {
    return { ...defaultConfig, mode: "wifi", apiBase: api };
  }

  try {
    const raw = window.localStorage.getItem(configKey);
    if (!raw) return defaultConfig;
    const parsed = JSON.parse(raw) as Partial<ConnectionConfig>;
    return {
      mode: parsed.mode === "serial" ? "serial" : "wifi",
      apiBase: typeof parsed.apiBase === "string" ? parsed.apiBase : "",
      serialBaud: typeof parsed.serialBaud === "number" ? parsed.serialBaud : 115200,
    };
  } catch {
    return defaultConfig;
  }
}

export function saveConnectionConfig(config: ConnectionConfig): void {
  window.localStorage.setItem(configKey, JSON.stringify(config));
}

function qs(params: Record<string, string | number | boolean>): string {
  const out = new URLSearchParams();
  for (const [key, value] of Object.entries(params)) {
    out.set(key, String(value));
  }
  return out.toString();
}

export class HttpTeamApi implements TeamApi {
  constructor(private readonly baseUrl = "") {}

  private async configureDeviceNow(command: DeviceConfigCommand): Promise<void> {
    const result = await this.configureDevice(command);
    if (!result.ok) {
      throw new Error(`config ${result.action} failed ret=${result.ret}`);
    }
  }

  getStatus(): Promise<TeamStatus | UnconfiguredStatus> {
    return fetchJson(`${this.baseUrl}/api/status`);
  }

  getNodes(): Promise<TeamNode[]> {
    return fetchJson(`${this.baseUrl}/api/nodes`);
  }

  getEvents(): Promise<TeamEvent[]> {
    return fetchJson(`${this.baseUrl}/api/events`);
  }

  getPending(): Promise<PendingMember[]> {
    return fetchJson(`${this.baseUrl}/api/pending`);
  }

  configureRole(command: RoleCommand): Promise<void> {
    return this.configureDeviceNow({ ...command, applyNow: true });
  }

  configurePairing(command: PairingCommand): Promise<void> {
    if (command.action === "approve") {
      return fetchAction(
        `${this.baseUrl}/api/pairing?${qs({
          action: "approve",
          id: command.id,
          relay: command.relay ? 1 : 0,
        })}`,
      );
    }
    return fetchAction(`${this.baseUrl}/api/pairing?action=${command.action}`);
  }

  selectMemberLeader(command: MemberSelectCommand): Promise<void> {
    return fetchAction(
      `${this.baseUrl}/api/member/select?${qs({
        team: command.teamId,
        leader: command.leaderSuffix,
        channel: command.channel,
      })}`,
    );
  }

  leaveMember(): Promise<void> {
    return fetchAction(`${this.baseUrl}/api/member/leave`);
  }

  factoryReset(): Promise<void> {
    return fetchAction(`${this.baseUrl}/api/factory-reset`);
  }

  send(command: SendCommand): Promise<TeamEvent> {
    if (command.type === "position") {
      const lat = ((command.latitudeE6 ?? 0) / 1_000_000).toFixed(6);
      const lon = ((command.longitudeE6 ?? 0) / 1_000_000).toFixed(6);
      return fetchAction(
        `${this.baseUrl}/api/location?${qs({
          lat,
          lon,
          dst: command.dstId,
          speed: command.speedCms ?? 0,
          heading: command.headingDeg ?? 0,
          battery: command.batteryPercent ?? 100,
          fix: command.fixStatus ?? 1,
          sat: command.satCount ?? 0,
        })}`,
      ).then(() => ({
        id: `http-location-${Date.now()}`,
        time: new Date().toISOString(),
        direction: "tx",
        type: "POS_REPORT",
        dstId: command.dstId,
        summary: `phone location lat=${lat} lon=${lon}`,
      }));
    }
    return fetchJson(`${this.baseUrl}/api/send`, {
      method: "POST",
      body: JSON.stringify(command),
    });
  }

  configureAllow(_command: AllowMembersCommand): Promise<TeamEvent> {
    return Promise.reject(new Error("WiFi HTTP 暂未提供配置写入接口，请切到串口模式执行成员准入配置"));
  }

  getDeviceConfig(): Promise<DeviceConfigStatus> {
    return fetchJson(`${this.baseUrl}/api/config/status`);
  }

  configureDevice(command: DeviceConfigCommand): Promise<DeviceConfigResult> {
    if (command.role === "leader") {
      return fetchJson(
        `${this.baseUrl}/api/config/leader?${qs({
          team: command.teamId,
          channel: command.channel,
          now: command.applyNow ? 1 : 0,
        })}`,
      );
    }
    return fetchJson(
      `${this.baseUrl}/api/config/member?${qs({
        leader: command.leaderSuffix,
        team: command.teamId,
        channel: command.channel,
        now: command.applyNow ? 1 : 0,
      })}`,
    );
  }

  applyDeviceConfig(): Promise<DeviceConfigResult> {
    return fetchJson(`${this.baseUrl}/api/config/apply`);
  }

  clearDeviceConfig(): Promise<DeviceConfigResult> {
    return fetchJson(`${this.baseUrl}/api/config/clear`);
  }

  rebootDevice(): Promise<DeviceConfigResult> {
    return fetchJson(`${this.baseUrl}/api/config/reboot`);
  }
}

type SerialPortLike = {
  readable: ReadableStream<Uint8Array> | null;
  writable: WritableStream<Uint8Array> | null;
  open(options: { baudRate: number }): Promise<void>;
};

export interface SerialDebugState {
  connected: boolean;
  lines: string[];
}

type SerialNavigator = Navigator & {
  serial?: {
    requestPort(options?: unknown): Promise<SerialPortLike>;
  };
};

type SerialLogEntry = {
  seq: number;
  line: string;
};

let selectedSerialPort: SerialPortLike | undefined;
let serialQueue: Promise<unknown> = Promise.resolve();
let serialReaderPort: SerialPortLike | undefined;
let serialReaderTask: Promise<void> | undefined;
let serialReadRemainder = "";
let serialLineSeq = 0;
const serialLines: SerialLogEntry[] = [];

function rememberSerialLines(lines: string[]): void {
  if (lines.length === 0) return;
  for (const line of lines) {
    serialLineSeq += 1;
    serialLines.push({ seq: serialLineSeq, line });
  }
  if (serialLines.length > 400) {
    serialLines.splice(0, serialLines.length - 400);
  }
}

function clearSerialLines(): void {
  serialLines.splice(0, serialLines.length);
  serialReadRemainder = "";
  serialLineSeq = 0;
}

function serialLinesSince(seq: number): string[] {
  return serialLines.filter((entry) => entry.seq > seq).map((entry) => entry.line);
}

function serialTextToLines(text: string): string[] {
  if (text.length === 0) return [];
  serialReadRemainder += text;
  const parts = serialReadRemainder.split(/\r?\n/);
  serialReadRemainder = parts.pop() ?? "";
  return parts.map((line) => line.trim()).filter(Boolean);
}

function flushSerialRemainder(): string[] {
  const line = serialReadRemainder.trim();
  serialReadRemainder = "";
  return line.length > 0 ? [line] : [];
}

function delay(ms: number): Promise<void> {
  return new Promise((resolve) => window.setTimeout(resolve, ms));
}

function serialCommandComplete(command: string, lines: string[]): boolean {
  const hasCfgJson = lines.some((line) => line.includes("[cfg-json]"));
  const hasCfgRet = lines.some((line) => line.startsWith("[cfg]") && /\bret=-?\d+/.test(line));
  if (command === "cfg status") return hasCfgJson;
  if (command.startsWith("cfg ")) return hasCfgJson && hasCfgRet;
  if (command === "state") return lines.some((line) => /team=\d+\s+self=\d+/.test(line)) || hasCfgJson;
  if (command === "members") return lines.some((line) => /\b(member|node)=\d+/.test(line));
  if (command === "allow") return lines.some((line) => /\ballow\b/.test(line));
  if (command.startsWith("pairing ")) return lines.some((line) => /\b(pairing|pending)\b/.test(line));
  return false;
}

async function waitForSerialLinesSince(seq: number, waitMs: number, command: string): Promise<string[]> {
  const deadline = Date.now() + waitMs;

  while (Date.now() < deadline) {
    await delay(Math.min(50, Math.max(0, deadline - Date.now())));
    const lines = serialLinesSince(seq).filter((line) => line !== `[cli-tx] ${command}`);
    if (serialCommandComplete(command, lines)) {
      break;
    }
  }

  return serialLinesSince(seq);
}

async function readSerialLoop(port: SerialPortLike): Promise<void> {
  if (!port.readable) return;
  const reader = port.readable.getReader();
  const decoder = new TextDecoder();
  try {
    while (serialReaderPort === port) {
      const result = await reader.read();
      if (result.done) break;
      rememberSerialLines(serialTextToLines(decoder.decode(result.value, { stream: true })));
    }
    rememberSerialLines(serialTextToLines(decoder.decode()));
    rememberSerialLines(flushSerialRemainder());
  } finally {
    reader.releaseLock();
  }
}

function startSerialReader(port: SerialPortLike): void {
  if (serialReaderPort === port && serialReaderTask) return;
  serialReaderPort = port;
  serialReaderTask = readSerialLoop(port)
    .catch((error: unknown) => {
      const message = error instanceof Error ? error.message : String(error);
      rememberSerialLines([`[cli-rx] serial reader stopped: ${message}`]);
    })
    .finally(() => {
      if (serialReaderPort === port) {
        serialReaderPort = undefined;
        serialReaderTask = undefined;
      }
    });
}

export function getSerialDebugState(): SerialDebugState {
  return {
    connected: selectedSerialPort?.readable != null && selectedSerialPort?.writable != null,
    lines: serialLines.map((entry) => entry.line),
  };
}

export async function requestSerialPort(baudRate: number): Promise<void> {
  const serialNavigator = navigator as SerialNavigator;
  if (!serialNavigator.serial) {
    throw new Error("当前浏览器不支持 WebSerial。请用 Chrome 或 Edge，并通过 HTTPS 打开页面。");
  }
  selectedSerialPort = await serialNavigator.serial.requestPort();
  await selectedSerialPort.open({ baudRate });
  clearSerialLines();
  startSerialReader(selectedSerialPort);
}

function cliStateToStatus(line: string): TeamStatus | undefined {
  const match = line.match(
    /team=(\d+)\s+self=(\d+)\s+leader=(\d+)\s+role=(\d+)\s+state=(\d+)\s+joined=(\d+)\s+seq=(\d+)/,
  );
  if (!match) return undefined;
  const state = Number(match[5]);
  const pairingMatch = line.match(/pairing=(\d+)/);
  const allowMatch = line.match(/allow=(all|only)\s+allow_count=(\d+)/);
  return {
    configured: true,
    teamId: Number(match[1]),
    selfId: Number(match[2]),
    leaderId: Number(match[3]),
    role: Number(match[4]) === 1 ? "leader" : "member",
    state: state === 3 ? "online" : state === 2 ? "joining" : state === 1 ? "wait_policy" : "idle",
    joined: Number(match[6]) !== 0,
    nextSeq: Number(match[7]),
    uptimeS: 0,
    transport: "serial",
    pairingEnabled: pairingMatch ? Number(pairingMatch[1]) !== 0 : undefined,
    memberFilterEnabled: allowMatch?.[1] === "only",
    allowedMemberCount: allowMatch ? Number(allowMatch[2]) : 0,
    allowedMembers: [],
  };
}

function cliMemberToNode(line: string): TeamNode | undefined {
  const match = line.match(
    /member=(\d+).*?\brole=(\d+).*?\bonline=(\d+)(?:.*?\bpending=(\d+))?.*?\bbattery=(\d+).*?\bfix=(\d+)(?:.*?\bpos_valid=(\d+))?.*?\blat=(-?\d+).*?\blon=(-?\d+).*?\bspeed=(\d+).*?\bheading=(\d+).*?\bsat=(\d+).*?\brssi=(-?\d+).*?\blast_seq=(\d+).*?\blast_seen=(\d+)/,
  );
  if (!match) return undefined;
  const labelMatch = line.match(/\blabel=(M[0-9A-Fa-f]{4})\b/);
  const macMatch = line.match(/\bmac=([0-9A-Fa-f]{4})\s+ready=(\d+)/);
  const relayMatch = line.match(/\brelay=(\d+)/);
  const tierMatch = line.match(/\btier=(\d+)/);
  const maxDownMatch = line.match(/\bmax_down=(\d+)/);
  const parentMatch = line.match(/\bparent=(\d+)/);
  const nextHopMatch = line.match(/\bnext_hop=(\d+)/);
  const childCountMatch = line.match(/\bchild_count=(\d+)/);
  const macReady = macMatch ? Number(macMatch[2]) !== 0 : undefined;
  const labelSuffix = labelMatch ? labelMatch[1].replace(/^M/, "").toUpperCase() : undefined;
  return {
    id: Number(match[1]),
    role: Number(match[2]) === 1 ? "leader" : "member",
    online: Number(match[3]) !== 0,
    policyPending: match[4] ? Number(match[4]) !== 0 : undefined,
    batteryPercent: Number(match[5]),
    fixStatus: Number(match[6]),
    lastRssiDbm: Number(match[13]) === 127 ? null : Number(match[13]),
    macReady,
    macSuffix: macReady ? macMatch?.[1].toUpperCase() : labelSuffix,
    relayAllowed: relayMatch ? Number(relayMatch[1]) !== 0 : undefined,
    relayTier: tierMatch ? Number(tierMatch[1]) : undefined,
    maxDownstream: maxDownMatch ? Number(maxDownMatch[1]) : undefined,
    parentId: parentMatch ? Number(parentMatch[1]) : undefined,
    nextHopId: nextHopMatch ? Number(nextHopMatch[1]) : undefined,
    childCount: childCountMatch ? Number(childCountMatch[1]) : undefined,
    lastSeq: Number(match[14]),
    lastSeenS: Number(match[15]),
    positionValid: match[7] ? Number(match[7]) !== 0 : Number(match[6]) !== 0,
    latitudeE6: Number(match[8]),
    longitudeE6: Number(match[9]),
    speedCms: Number(match[10]),
    headingDeg: Number(match[11]),
    satCount: Number(match[12]),
  };
}

function cliPendingToMember(line: string): PendingMember | undefined {
  const match = line.match(/pending member=(\d+)\s+role=(\d+)\s+battery=(\d+)\s+mac=([0-9A-Fa-f]{4})\s+ready=(\d+)\s+last_seen=(\d+)/);
  if (!match) return undefined;
  return {
    id: Number(match[1]),
    role: Number(match[2]) === 1 ? "leader" : "member",
    batteryPercent: Number(match[3]),
    macReady: Number(match[5]) !== 0,
    macSuffix: match[4].toUpperCase(),
    lastSeenS: Number(match[6]),
  };
}

function defaultConfigStatus(): DeviceConfigStatus {
  return {
    ok: false,
    selfSuffix: "0000",
    routeId: 0,
    nvValid: false,
    nvRole: "none",
    nvRoleValue: 255,
    nvTeam: 0,
    nvChannel: 0,
    nvLeaderSuffix: "0000",
    nvLeaderTerm: 0,
    runtimeConfigured: false,
    runtimeRole: "none",
    runtimeRoleValue: 255,
    runtimeTeam: 0,
    runtimeChannel: 0,
    runtimeLeader: 0,
    runtimeLeaderTerm: 0,
    runtimeSelf: 0,
    runtimeDirectCap: 0,
    runtimeRelayTarget: 0,
    runtimeRelayCount: 0,
    runtimeOnlineCount: 0,
    runtimeJoined: 0,
    runtimeParent: 0,
    runtimeRelayEnabled: 0,
    roleRequestPending: false,
    roleRequestRole: "none",
    roleRequestRoleValue: 255,
    roleRequestTeam: 0,
    roleRequestChannel: 0,
    roleRequestLeader: 0,
    roleRequestLeaderSuffix: "0000",
    roleRequestLeaderTerm: 0,
    roleRequestLastRet: 0,
    lastRoleRet: 0,
  };
}

function isConfigRole(value: unknown): value is DeviceConfigStatus["nvRole"] {
  return value === "leader" || value === "member" || value === "none";
}

function normalizeSuffix(value: unknown): string {
  const suffix = typeof value === "string" ? value.trim().toUpperCase() : "";
  return /^[0-9A-F]{4}$/.test(suffix) ? suffix : "0000";
}

function normalizeConfigStatus(value: unknown): DeviceConfigStatus {
  const input = typeof value === "object" && value !== null ? (value as Record<string, unknown>) : {};
  const fallback = defaultConfigStatus();
  const numberField = (key: keyof DeviceConfigStatus): number => {
    const raw = input[key];
    return typeof raw === "number" && Number.isFinite(raw) ? raw : (fallback[key] as number);
  };
  const booleanField = (key: keyof DeviceConfigStatus): boolean => {
    const raw = input[key];
    return typeof raw === "boolean" ? raw : (fallback[key] as boolean);
  };
  const roleField = (key: keyof DeviceConfigStatus): DeviceConfigStatus["nvRole"] => {
    const raw = input[key];
    return isConfigRole(raw) ? raw : "none";
  };
  return {
    ok: booleanField("ok"),
    fw: typeof input.fw === "string" ? input.fw : undefined,
    selfSuffix: normalizeSuffix(input.selfSuffix),
    routeId: numberField("routeId"),
    nvValid: booleanField("nvValid"),
    nvRole: roleField("nvRole"),
    nvRoleValue: numberField("nvRoleValue"),
    nvTeam: numberField("nvTeam"),
    nvChannel: numberField("nvChannel"),
    nvLeaderSuffix: normalizeSuffix(input.nvLeaderSuffix),
    nvLeaderTerm: numberField("nvLeaderTerm"),
    runtimeConfigured: booleanField("runtimeConfigured"),
    runtimeRole: roleField("runtimeRole"),
    runtimeRoleValue: numberField("runtimeRoleValue"),
    runtimeTeam: numberField("runtimeTeam"),
    runtimeChannel: numberField("runtimeChannel"),
    runtimeLeader: numberField("runtimeLeader"),
    runtimeLeaderTerm: numberField("runtimeLeaderTerm"),
    runtimeSelf: numberField("runtimeSelf"),
    runtimeDirectCap: numberField("runtimeDirectCap"),
    runtimeRelayTarget: numberField("runtimeRelayTarget"),
    runtimeRelayCount: numberField("runtimeRelayCount"),
    runtimeOnlineCount: numberField("runtimeOnlineCount"),
    runtimeJoined: numberField("runtimeJoined"),
    runtimeParent: numberField("runtimeParent"),
    runtimeRelayEnabled: numberField("runtimeRelayEnabled"),
    roleRequestPending: booleanField("roleRequestPending"),
    roleRequestRole: roleField("roleRequestRole"),
    roleRequestRoleValue: numberField("roleRequestRoleValue"),
    roleRequestTeam: numberField("roleRequestTeam"),
    roleRequestChannel: numberField("roleRequestChannel"),
    roleRequestLeader: numberField("roleRequestLeader"),
    roleRequestLeaderSuffix: normalizeSuffix(input.roleRequestLeaderSuffix),
    roleRequestLeaderTerm: numberField("roleRequestLeaderTerm"),
    roleRequestLastRet: numberField("roleRequestLastRet"),
    lastRoleRet: numberField("lastRoleRet"),
  };
}

function parseConfigStatusLine(line: string): DeviceConfigStatus | undefined {
  const prefix = "[cfg-json]";
  if (!line.startsWith(prefix)) return undefined;
  try {
    return normalizeConfigStatus(JSON.parse(line.slice(prefix.length).trim()));
  } catch {
    return undefined;
  }
}

function latestConfigStatus(lines: string[]): DeviceConfigStatus | undefined {
  for (const line of [...lines].reverse()) {
    const parsed = parseConfigStatusLine(line);
    if (parsed) return parsed;
  }
  return undefined;
}

function latestCfgRet(lines: string[]): number | undefined {
  for (const line of [...lines].reverse()) {
    if (!line.startsWith("[cfg]")) continue;
    const match = line.match(/\bret=(-?\d+)/);
    if (match) return Number(match[1]);
  }
  return undefined;
}

function configResultRet(lines: string[], fallback: number): number {
  return latestCfgRet(lines) ?? fallback;
}

function configCommandToCli(command: DeviceConfigCommand): string {
  const now = command.applyNow ? " now" : "";
  if (command.role === "leader") {
    return `cfg leader${now} ${command.teamId} ${command.channel}`;
  }
  return `cfg member${now} ${command.leaderSuffix} ${command.teamId} ${command.channel}`;
}

function configStatusToUnconfiguredStatus(config: DeviceConfigStatus): UnconfiguredStatus {
  return {
    configured: false,
    selfLabel: `SLE-${config.selfSuffix}`,
    routeId: config.routeId,
    macReady: config.selfSuffix !== "0000",
    macSuffix: config.selfSuffix,
    ssid: "serial",
    transport: "serial",
    roleRequestPending: config.roleRequestPending,
    roleRequestRole: config.roleRequestRole,
    roleRequestRoleValue: config.roleRequestRoleValue,
    roleRequestTeam: config.roleRequestTeam,
    roleRequestChannel: config.roleRequestChannel,
    roleRequestLeader: config.roleRequestLeader,
    roleRequestLeaderSuffix: config.roleRequestLeaderSuffix,
    roleRequestLeaderTerm: config.roleRequestLeaderTerm,
    roleRequestLastRet: config.roleRequestLastRet,
  };
}

function configStatusToRuntimeStatus(config: DeviceConfigStatus): TeamStatus | UnconfiguredStatus {
  if (!config.runtimeConfigured || (config.runtimeRole !== "leader" && config.runtimeRole !== "member")) {
    return configStatusToUnconfiguredStatus(config);
  }
  return {
    configured: true,
    selfLabel: `SLE-${config.selfSuffix}`,
    routeId: config.routeId,
    macSuffix: config.selfSuffix,
    teamId: config.runtimeTeam,
    selfId: config.runtimeSelf,
    leaderId: config.runtimeLeader,
    role: config.runtimeRole,
    state: config.runtimeRole === "leader" ? "online" : config.runtimeJoined !== 0 ? "online" : "wait_policy",
    joined: config.runtimeRole === "leader" || config.runtimeJoined !== 0,
    maxDownstream: config.runtimeDirectCap,
    relayEnabled: config.runtimeRelayEnabled !== 0,
    upstreamParentId: config.runtimeParent,
    nextSeq: 0,
    uptimeS: 0,
    transport: "serial",
  };
}

function routeIdFromSuffix(suffix: string): number {
  const value = Number.parseInt(suffix, 16) & 0xffff;
  const mix = (value & 0xff) + (((value >> 8) & 0xff) * 31);
  return (mix % 254) + 1;
}

function sendCommandToCli(command: SendCommand): string {
  if (command.type === "heartbeat") {
    return `hb ${command.dstId} ${command.batteryPercent ?? 100} ${command.rssiDbm ?? 127} ${command.fixStatus ?? 1}`;
  }
  if (command.type === "position") {
    return `pos ${command.dstId} ${command.latitudeE6 ?? 0} ${command.longitudeE6 ?? 0} ${command.speedCms ?? 0} ${command.headingDeg ?? 0} ${command.batteryPercent ?? 100} ${command.fixStatus ?? 0} ${command.satCount ?? 0}`;
  }
  if (command.type === "alert") {
    return `alert ${command.dstId} ${command.lostMemberId ?? 0} ${command.reason ?? 0} ${command.latitudeE6 ?? 0} ${command.longitudeE6 ?? 0} ${command.lastReportS ?? 0}`;
  }
  return `config ${command.dstId}`;
}

function allowCommandToCli(command: AllowMembersCommand): string {
  if (command.mode === "all") {
    return "allow all";
  }
  const ids = (command.memberIds ?? []).map((id) => Math.trunc(id)).filter((id) => id >= 1 && id <= 254);
  if (command.mode === "only") {
    if (ids.length === 0) throw new Error("allow only 需要至少一个 member id");
    return `allow only ${ids.join(" ")}`;
  }
  if (ids.length !== 1) throw new Error("allow add/del 每次只处理一个 member id");
  return `allow ${command.mode} ${ids[0]}`;
}

function appTypeFromText(text: string): TeamEvent["type"] {
  const upper = text.toUpperCase();
  if (upper.includes("HEARTBEAT")) return "HEARTBEAT";
  if (upper.includes("POS_REPORT") || upper.includes("POSITION") || upper.includes(" POS ")) return "POS_REPORT";
  if (upper.includes("ALERT")) return "ALERT";
  if (upper.includes("CONFIG")) return "CONFIG";
  if (upper.includes("HELLO")) return "HELLO";
  if (upper.includes("ACK")) return "ACK";
  if (upper.includes("PACKET")) return "PACKET";
  if (text.startsWith("[cli-tx]") || text.startsWith("[cli-rx]") || text.startsWith("[cli]")) return "CLI";
  if (text.startsWith("[state]") || text.startsWith("[team]")) return "STATE";
  if (text.startsWith("[team-wifi]")) return "SYSTEM";
  return "UNKNOWN";
}

function cleanTaggedLine(line: string): string {
  return line.replace(/^\[[^\]]+\]\s*/, "");
}

function serialLineToEvent(line: string, index: number): TeamEvent {
  const cleaned = cleanTaggedLine(line);
  const base = {
    id: `serial-line-${Date.now()}-${index}`,
    time: new Date().toISOString(),
    type: appTypeFromText(line),
    summary: cleaned,
  };
  if (line.startsWith("[sle-tx-ok]")) {
    return { ...base, direction: "tx", summary: `SLE发送成功：${cleaned}` };
  }
  if (line.startsWith("[sle-tx-fail]")) {
    return { ...base, direction: "fail", summary: `SLE发送失败：${cleaned}` };
  }
  if (line.startsWith("[sle-rx]")) {
    return { ...base, direction: "rx", summary: `SLE收到：${cleaned}` };
  }
  if (line.startsWith("[cli-tx]")) {
    return { ...base, direction: "cli", summary: `串口命令：${cleaned}` };
  }
  if (line.startsWith("[cli-rx]") || line.startsWith("[cli]")) {
    return { ...base, direction: "cli", summary: `串口返回：${cleaned}` };
  }
  if (line.startsWith("[state]") || line.startsWith("[team]")) {
    return { ...base, direction: "state", summary: `状态：${cleaned}` };
  }
  if (line.startsWith("[team-wifi]")) {
    return { ...base, direction: "state", summary: `WiFi状态：${cleaned}` };
  }
  return { ...base, direction: "system", summary: line };
}

export class SerialTeamApi implements TeamApi {
  private lastLines: string[] = [];

  constructor(private readonly baudRate = 115200) {}

  private async configureDeviceNow(command: DeviceConfigCommand): Promise<void> {
    const result = await this.configureDevice(command);
    if (!result.ok) {
      throw new Error(`config ${result.action} failed ret=${result.ret}`);
    }
  }

  private async ensurePort(): Promise<SerialPortLike> {
    if (!selectedSerialPort) {
      await requestSerialPort(this.baudRate);
    }
    if (!selectedSerialPort?.readable || !selectedSerialPort.writable) {
      throw new Error("串口未打开");
    }
    startSerialReader(selectedSerialPort);
    return selectedSerialPort;
  }

  private async runCliUnlocked(command: string, waitMs = 450): Promise<string[]> {
    const port = await this.ensurePort();
    const startSeq = serialLineSeq;
    const txLine = `[cli-tx] ${command}`;
    const writer = port.writable!.getWriter();
    try {
      this.lastLines = [...this.lastLines, txLine].slice(-80);
      rememberSerialLines([txLine]);
      await writer.write(new TextEncoder().encode(`${command}\r\n`));
    } finally {
      writer.releaseLock();
    }

    const lines = (await waitForSerialLinesSince(startSeq, waitMs, command)).filter((line) => line !== txLine);
    this.lastLines = [...this.lastLines, ...lines].slice(-80);
    return lines;
  }

  private runCli(command: string, waitMs = 450): Promise<string[]> {
    const task = serialQueue.then(() => this.runCliUnlocked(command, waitMs));
    serialQueue = task.catch(() => undefined);
    return task;
  }

  async getStatus(): Promise<TeamStatus | UnconfiguredStatus> {
    const lines = await this.runCli("state");
    const status = lines.map(cliStateToStatus).find(Boolean);
    if (!status) {
      return configStatusToRuntimeStatus(await this.getDeviceConfig());
    }
    if (status.memberFilterEnabled) {
      const allowLines = await this.runCli("allow");
      status.allowedMembers = allowLines
        .map((line) => line.match(/allow member=(\d+)/)?.[1])
        .filter((value): value is string => value !== undefined)
        .map(Number);
      status.allowedMemberCount = status.allowedMembers.length;
    }
    return status;
  }

  async getNodes(): Promise<TeamNode[]> {
    const lines = await this.runCli("members");
    return lines.map(cliMemberToNode).filter((node): node is TeamNode => node !== undefined);
  }

  async getEvents(): Promise<TeamEvent[]> {
    return this.lastLines
      .slice(-20)
      .reverse()
      .map((line, index) => serialLineToEvent(line, index));
  }

  async getPending(): Promise<PendingMember[]> {
    const lines = await this.runCli("pairing pending");
    return lines.map(cliPendingToMember).filter((member): member is PendingMember => member !== undefined);
  }

  async configureRole(command: RoleCommand): Promise<void> {
    await this.configureDeviceNow({ ...command, applyNow: true });
  }

  async configurePairing(command: PairingCommand): Promise<void> {
    if (command.action === "approve") {
      await this.runCli(`pairing approve ${command.id} ${command.relay ? "relay" : "norelay"}`, 800);
      return;
    }
    await this.runCli(`pairing ${command.action}`, 800);
  }

  async selectMemberLeader(command: MemberSelectCommand): Promise<void> {
    await this.runCli(`join ${command.teamId} ${routeIdFromSuffix(command.leaderSuffix)} ${command.channel}`, 800);
  }

  async leaveMember(): Promise<void> {
    await this.runCli("leave", 800);
  }

  async factoryReset(): Promise<void> {
    return Promise.reject(new Error("串口模式暂无 factory reset CLI；请用 WiFi HTTP /api/factory-reset 或串口 leave 后重新配置"));
  }

  async send(command: SendCommand): Promise<TeamEvent> {
    const cli = sendCommandToCli(command);
    await this.runCli(cli, 700);
    const latest = [...this.lastLines].reverse().find((line) => line.includes("[sle-tx-")) ?? `[cli-tx] ${cli}`;
    return serialLineToEvent(latest, Date.now());
  }

  async configureAllow(command: AllowMembersCommand): Promise<TeamEvent> {
    const cli = allowCommandToCli(command);
    await this.runCli(cli, 500);
    const latest = [...this.lastLines].reverse().find((line) => line.includes("allow ")) ?? `[cli-tx] ${cli}`;
    return serialLineToEvent(latest, Date.now());
  }

  async getDeviceConfig(): Promise<DeviceConfigStatus> {
    const lines = await this.runCli("cfg status", 800);
    const parsed = latestConfigStatus(lines);
    if (!parsed) {
      throw new Error("serial cfg status did not return [cfg-json]");
    }
    return parsed;
  }

  async configureDevice(command: DeviceConfigCommand): Promise<DeviceConfigResult> {
    const cli = configCommandToCli(command);
    const lines = await this.runCli(cli, command.applyNow ? 1400 : 800);
    const config = latestConfigStatus(lines) ?? (await this.getDeviceConfig());
    const ret = configResultRet(lines, -1);
    return { ok: ret === 0, action: command.applyNow ? `${command.role}-now` : command.role, ret, config };
  }

  async applyDeviceConfig(): Promise<DeviceConfigResult> {
    const lines = await this.runCli("cfg apply", 1400);
    const config = latestConfigStatus(lines) ?? (await this.getDeviceConfig());
    const ret = configResultRet(lines, -1);
    return { ok: ret === 0, action: "apply", ret, config };
  }

  async clearDeviceConfig(): Promise<DeviceConfigResult> {
    const lines = await this.runCli("cfg clear", 800);
    const config = latestConfigStatus(lines) ?? (await this.getDeviceConfig());
    const ret = latestCfgRet(lines) ?? (config.nvValid ? -4 : 0);
    return { ok: ret === 0 && !config.nvValid, action: "clear", ret, config };
  }

  async rebootDevice(): Promise<DeviceConfigResult> {
    await this.runCli("cfg reboot", 300);
    const config = latestConfigStatus(this.lastLines) ?? defaultConfigStatus();
    return { ok: true, action: "reboot", ret: 0, config };
  }
}

export function createTeamApi(): TeamApi {
  const config = loadConnectionConfig();
  if (config.mode === "serial") {
    return new SerialTeamApi(config.serialBaud);
  }
  return new HttpTeamApi(config.apiBase);
}
