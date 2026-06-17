export type NodeRole = "leader" | "member";
export type NodeState = "idle" | "wait_policy" | "joining" | "online";
export type ParentState = "idle" | "wait_policy" | "connected";

export type AppMessageType =
  | "HELLO"
  | "HEARTBEAT"
  | "POS_REPORT"
  | "ALERT"
  | "CONFIG"
  | "ACK"
  | "ROUTE_UPDATE"
  | "PACKET"
  | "CLI"
  | "STATE"
  | "SYSTEM"
  | "UNKNOWN";

export interface TeamStatus {
  configured?: boolean;
  selfLabel?: string;
  routeId?: number;
  macReady?: boolean;
  mac?: string;
  macSuffix?: string;
  ssid?: string;
  teamId: number;
  selfId: number;
  leaderId: number;
  role: NodeRole;
  state: NodeState;
  joined: boolean;
  relayAllowed?: boolean;
  relayEnabled?: boolean;
  relayTier?: number;
  maxDownstream?: number;
  upstreamParentId?: number;
  upstreamParentState?: ParentState;
  nextSeq: number;
  uptimeS: number;
  transport: "ws63-softap" | "ws63-http" | "hosted-http" | "serial";
  pairingEnabled?: boolean;
  memberFilterEnabled?: boolean;
  allowedMemberCount?: number;
  allowedMembers?: number[];
}

export interface UnconfiguredStatus {
  configured: false;
  selfLabel: string;
  routeId: number;
  macReady: boolean;
  macSuffix: string;
  ssid: string;
  transport: "ws63-softap" | "ws63-http" | "hosted-http" | "serial";
}

export interface TeamNode {
  id: number;
  role: NodeRole;
  online: boolean;
  policyPending?: boolean;
  batteryPercent: number;
  fixStatus: number;
  lastRssiDbm: number | null;
  macReady?: boolean;
  mac?: string;
  macSuffix?: string;
  relayAllowed?: boolean;
  relayTier?: number;
  maxDownstream?: number;
  parentId?: number;
  nextHopId?: number;
  childCount?: number;
  lastSeq: number;
  lastSeenS: number;
  positionValid?: boolean;
  latitudeE6?: number;
  longitudeE6?: number;
  speedCms?: number;
  headingDeg?: number;
  satCount?: number;
}

export interface PendingMember {
  id: number;
  role: NodeRole;
  batteryPercent: number;
  macReady: boolean;
  mac?: string;
  macSuffix: string;
  lastSeenS: number;
}

export interface TeamEvent {
  id: string;
  time: string;
  direction: "rx" | "tx" | "fail" | "cli" | "state" | "system";
  type: AppMessageType;
  srcId?: number;
  dstId?: number;
  seq?: number;
  summary: string;
  rawHex?: string;
}

export interface PacketDecodeResult {
  ok: boolean;
  error?: string;
  mesh?: {
    version: number;
    payloadType: number;
    routeType: number;
    pathHashSize: number;
    hopCount: number;
    payloadLen: number;
    channelHash?: number;
    cipherMac?: string;
  };
  app?: {
    type: AppMessageType;
    typeValue: number;
    flags: number;
    seq: number;
    teamId: number;
    srcId: number;
    dstId: number;
    ttl: number;
    bodyLen: number;
    body: Record<string, number>;
  };
}

export interface SendCommand {
  type: "heartbeat" | "position" | "alert" | "config";
  dstId: number;
  batteryPercent?: number;
  rssiDbm?: number;
  fixStatus?: number;
  latitudeE6?: number;
  longitudeE6?: number;
  speedCms?: number;
  headingDeg?: number;
  satCount?: number;
  lostMemberId?: number;
  reason?: number;
  lastReportS?: number;
}

export interface AllowMembersCommand {
  mode: "all" | "only" | "add" | "del";
  memberIds?: number[];
}

export type ConfigRole = NodeRole | "none";

export interface DeviceConfigStatus {
  ok: boolean;
  fw?: string;
  selfSuffix: string;
  routeId: number;
  nvValid: boolean;
  nvRole: ConfigRole;
  nvRoleValue: number;
  nvTeam: number;
  nvChannel: number;
  nvLeaderSuffix: string;
  nvLeaderTerm: number;
  runtimeConfigured: boolean;
  runtimeRole: ConfigRole;
  runtimeRoleValue: number;
  runtimeTeam: number;
  runtimeChannel: number;
  runtimeLeader: number;
  runtimeLeaderTerm: number;
  runtimeSelf: number;
  runtimeDirectCap: number;
  runtimeRelayTarget: number;
  runtimeRelayCount: number;
  runtimeOnlineCount: number;
  runtimeJoined: number;
  runtimeParent: number;
  runtimeRelayEnabled: number;
  lastRoleRet: number;
}

export type DeviceConfigCommand =
  | { role: "leader"; teamId: number; channel: number; applyNow: boolean }
  | { role: "member"; leaderSuffix: string; teamId: number; channel: number; applyNow: boolean };

export interface DeviceConfigResult {
  ok: boolean;
  action: string;
  ret: number;
  config: DeviceConfigStatus;
}

export type RoleCommand =
  | { role: "leader"; teamId: number; channel: number }
  | { role: "member"; leaderSuffix: string; teamId: number; channel: number };

export type PairingCommand =
  | { action: "start" }
  | { action: "stop" }
  | { action: "approve"; id: number; relay: boolean };

export interface MemberSelectCommand {
  teamId: number;
  leaderSuffix: string;
  channel: number;
}
