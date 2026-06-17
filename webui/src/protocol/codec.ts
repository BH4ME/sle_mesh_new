import type { AppMessageType, PacketDecodeResult } from "./types";

const GROUP_DATA = 0x06;
const APP_HEADER_SIZE = 10;

const appTypes = new Map<number, AppMessageType>([
  [0x01, "HELLO"],
  [0x02, "HEARTBEAT"],
  [0x03, "POS_REPORT"],
  [0x04, "ALERT"],
  [0x05, "CONFIG"],
  [0x06, "ACK"],
]);

export function normalizeHex(input: string): string {
  return input.replace(/0x/gi, "").replace(/[^0-9a-f]/gi, "").toUpperCase();
}

export function hexToBytes(input: string): Uint8Array {
  const hex = normalizeHex(input);
  if (hex.length === 0) return new Uint8Array();
  if (hex.length % 2 !== 0) {
    throw new Error("hex length must be even");
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i += 1) {
    out[i] = Number.parseInt(hex.slice(i * 2, i * 2 + 2), 16);
  }
  return out;
}

export function bytesToHex(bytes: Uint8Array): string {
  return Array.from(bytes)
    .map((value) => value.toString(16).padStart(2, "0").toUpperCase())
    .join(" ");
}

function u16le(bytes: Uint8Array, offset: number): number {
  return bytes[offset] | (bytes[offset + 1] << 8);
}

function i8(byte: number): number {
  return byte > 127 ? byte - 256 : byte;
}

function i32le(bytes: Uint8Array, offset: number): number {
  const value =
    bytes[offset] |
    (bytes[offset + 1] << 8) |
    (bytes[offset + 2] << 16) |
    (bytes[offset + 3] << 24);
  return value | 0;
}

function u32le(bytes: Uint8Array, offset: number): number {
  return (
    bytes[offset] +
    bytes[offset + 1] * 0x100 +
    bytes[offset + 2] * 0x10000 +
    bytes[offset + 3] * 0x1000000
  );
}

function decodeBody(typeValue: number, body: Uint8Array): Record<string, number> {
  switch (typeValue) {
    case 0x01:
      return {
        deviceId: body[0] ?? 0,
        role: body[1] ?? 0,
        batteryPercent: body[2] ?? 0,
      };
    case 0x02:
      return {
        batteryPercent: body[0] ?? 0,
        rssiDbm: i8(body[1] ?? 0),
        fixStatus: body[2] ?? 0,
      };
    case 0x03:
      return {
        latitudeE6: i32le(body, 0),
        longitudeE6: i32le(body, 4),
        speedCms: u16le(body, 8),
        headingDeg: u16le(body, 10),
        batteryPercent: body[12] ?? 0,
        fixStatus: body[13] ?? 0,
        satCount: body[14] ?? 0,
      };
    case 0x04:
      return {
        lostMemberId: body[0] ?? 0,
        reason: body[1] ?? 0,
        lastLatitudeE6: i32le(body, 4),
        lastLongitudeE6: i32le(body, 8),
        lastReportS: u32le(body, 12),
      };
    case 0x05:
      return {
        reportIntervalS: u16le(body, 0),
        warnDistanceM: u16le(body, 2),
        lostDistanceM: u16le(body, 4),
        heartbeatTimeoutS: u16le(body, 6),
      };
    case 0x06:
      return {
        ackSeq: u16le(body, 0),
        ackedMsgType: body[2] ?? 0,
        statusCode: body[3] ?? 0,
      };
    default:
      return {};
  }
}

export function decodePacketHex(input: string): PacketDecodeResult {
  try {
    const bytes = hexToBytes(input);
    if (bytes.length < 2) {
      return { ok: false, error: "packet too short" };
    }

    let offset = 0;
    const header = bytes[offset++];
    const routeType = header & 0x03;
    const payloadType = (header >> 2) & 0x0f;
    const version = (header >> 6) & 0x03;
    const hasTransport = routeType === 0x00 || routeType === 0x03;

    if (hasTransport) {
      if (bytes.length < offset + 4) return { ok: false, error: "transport header truncated" };
      offset += 4;
    }

    const pathLenByte = bytes[offset++];
    const hopCount = pathLenByte & 0x3f;
    const pathHashSize = ((pathLenByte >> 6) & 0x03) + 1;
    const pathBytes = hopCount * pathHashSize;
    if (bytes.length < offset + pathBytes) {
      return { ok: false, error: "path truncated" };
    }
    offset += pathBytes;

    const payload = bytes.slice(offset);
    const result: PacketDecodeResult = {
      ok: true,
      mesh: {
        version,
        payloadType,
        routeType,
        pathHashSize,
        hopCount,
        payloadLen: payload.length,
      },
    };

    if (payloadType !== GROUP_DATA) {
      return result;
    }
    if (payload.length < 3 + APP_HEADER_SIZE) {
      return { ok: false, error: "GROUP_DATA payload too short", mesh: result.mesh };
    }

    const channelHash = payload[0];
    const cipherMacBytes = payload.slice(1, 3);
    const app = payload.slice(3);
    if (result.mesh) {
      result.mesh.channelHash = channelHash;
      result.mesh.cipherMac = bytesToHex(cipherMacBytes);
    }

    const bodyLen = u16le(app, 8);
    if (app.length < APP_HEADER_SIZE + bodyLen) {
      return { ok: false, error: "app body truncated", mesh: result.mesh };
    }
    const typeValue = app[0];
    const body = app.slice(APP_HEADER_SIZE, APP_HEADER_SIZE + bodyLen);
    result.app = {
      type: appTypes.get(typeValue) ?? "UNKNOWN",
      typeValue,
      flags: app[1],
      seq: u16le(app, 2),
      teamId: app[4],
      srcId: app[5],
      dstId: app[6],
      ttl: app[7],
      bodyLen,
      body: decodeBody(typeValue, body),
    };
    return result;
  } catch (error) {
    return { ok: false, error: error instanceof Error ? error.message : "decode failed" };
  }
}

export function formatCoordinate(valueE6?: number): string {
  if (valueE6 === undefined) return "no fix";
  return (valueE6 / 1_000_000).toFixed(6);
}
