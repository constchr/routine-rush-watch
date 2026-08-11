// ─────────────────────────────────────────────────────────────
// RR_SYNC — GATT contract for the Routine Rush watch companion
//
// CONTRACT VERSION 2 — re-frozen 2026-08-11.
//   v2 adds RR_CONTROL (6th characteristic) and moves command traffic
//   (nonce_auth, factory_reset) off the ROUTINE_PUSH envelope onto it.
//   ROUTINE_PUSH is once again PURELY routine data.
//   This was a deliberate, coordinated amendment — the freeze exists to
//   prevent SILENT drift, not to prevent evolution. Both sides regenerated.
//
// Firmware spec §6B.3.  THIS FILE IS THE CONTRACT between two codebases:
// the phone (TypeScript, here) and the watch firmware (C, elsewhere) implement
// opposite ends of these exact byte layouts. Change nothing here without
// changing the firmware in lockstep.
//
// ── Conventions ──────────────────────────────────────────────
// • ENDIANNESS: all multi-byte integers are LITTLE-ENDIAN (u16/u32). This
//   matches the watch MCU (ARM Cortex-M) native byte order, so the firmware
//   can memcpy fixed structs directly. Every offset below is explicit.
// • UUIDs on the wire (RUN_ACK) are 16 bytes in CANONICAL order — the same
//   byte order you read the hex string in, left to right. NOT endian-swapped.
//   e.g. "00112233-4455-6677-8899-aabbccddeeff" → 00 11 22 33 44 55 … ee ff.
// • Variable-length payloads (QUEUE_PULL, ROUTINE_PUSH) are LENGTH-PREFIXED
//   UTF-8 JSON frames. The length prefix is itself little-endian. The BLE
//   transport (Part D) splits these across MTU-sized packets; the framing
//   here is transport-independent — decode once the full value is reassembled.
//
// All encoders return Uint8Array; all decoders take Uint8Array (or a slice).
// No react-native-ble-plx import here — this module is pure and unit-tested
// under plain Node. The BLEService adapts Uint8Array ↔ base64 for the radio.
// ─────────────────────────────────────────────────────────────

import type { WatchRunRecord } from '@routine-rush/shared'

// ── Service + characteristic UUIDs (generated with uuidgen) ──────────────────
export const RR_SYNC_SERVICE_UUID = 'fc19364a-c250-4477-928d-28c55ac1c2bd'

export const RR_SYNC_CHARACTERISTICS = {
  /** read | notify — fixed 9-byte status block. */
  QUEUE_STATUS: '4f42f9af-8944-4c5e-b5c0-1cf72cdaf9ac',
  /** read (multi-packet) — length-prefixed stream of queued run records. */
  QUEUE_PULL: '0cc5aba4-0c21-46ba-afcf-fac7f9d2aa73',
  /** write — fixed 38-byte authoritative ack for one relayed run. */
  RUN_ACK: '80d6e7d2-1921-4655-a48a-4a2f43aeeac2',
  /** write (multi-packet) — length-prefixed routines.json push. */
  ROUTINE_PUSH: '8f0956d2-1818-4961-b6ed-a88844f40933',
  /** write — fixed 4-byte epoch. */
  TIME_SYNC: '3c5a115d-61a0-4104-9883-ebe780044eb5',
  /** write (multi-packet) — length-prefixed command envelope. v2. */
  RR_CONTROL: '73c4f178-0884-4db4-9624-ff443355763b',
} as const

export type RrSyncCharacteristic = keyof typeof RR_SYNC_CHARACTERISTICS

// ── low-level UTF-8 — hand-rolled so this module depends on nothing but
// DataView/Uint8Array (no TextEncoder ambient type, no runtime polyfill). Handles
// the full BMP + surrogate pairs, so Greek (el locale) routines encode correctly.
function utf8Encode(str: string): Uint8Array {
  const out: number[] = []
  for (let i = 0; i < str.length; i++) {
    const c = str.charCodeAt(i)
    if (c < 0x80) out.push(c)
    else if (c < 0x800) out.push(0xc0 | (c >> 6), 0x80 | (c & 0x3f))
    else if (c >= 0xd800 && c <= 0xdbff) {
      const cp = 0x10000 + ((c & 0x3ff) << 10) + (str.charCodeAt(++i) & 0x3ff)
      out.push(0xf0 | (cp >> 18), 0x80 | ((cp >> 12) & 0x3f), 0x80 | ((cp >> 6) & 0x3f), 0x80 | (cp & 0x3f))
    } else out.push(0xe0 | (c >> 12), 0x80 | ((c >> 6) & 0x3f), 0x80 | (c & 0x3f))
  }
  return new Uint8Array(out)
}

function utf8Decode(bytes: Uint8Array): string {
  let out = '', i = 0
  while (i < bytes.byteLength) {
    const b = bytes[i++]
    if (b < 0x80) out += String.fromCharCode(b)
    else if (b < 0xe0) out += String.fromCharCode(((b & 0x1f) << 6) | (bytes[i++] & 0x3f))
    else if (b < 0xf0) out += String.fromCharCode(((b & 0x0f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f))
    else {
      const cp = (((b & 0x07) << 18) | ((bytes[i++] & 0x3f) << 12) | ((bytes[i++] & 0x3f) << 6) | (bytes[i++] & 0x3f)) - 0x10000
      out += String.fromCharCode(0xd800 + (cp >> 10), 0xdc00 + (cp & 0x3ff))
    }
  }
  return out
}

function view(bytes: Uint8Array): DataView {
  return new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength)
}

// ─────────────────────────────────────────────────────────────
// QUEUE_STATUS  (read | notify) — fixed 9 bytes, little-endian
//
//   offset  size  field          type   notes
//   0       2     queued_count   u16    runs waiting in the watch queue
//   2       4     oldest_ts      u32    epoch seconds of oldest queued run (0=none)
//   6       2     fw_version     u16    packed firmware version
//   8       1     batt           u8     battery percent 0..100
//   ── total 9 bytes ──
// ─────────────────────────────────────────────────────────────

export interface QueueStatus {
  queued_count: number
  oldest_ts: number
  fw_version: number
  batt: number
}

export const QUEUE_STATUS_SIZE = 9

export function encodeQueueStatus(s: QueueStatus): Uint8Array {
  assertU16('queued_count', s.queued_count)
  assertU32('oldest_ts', s.oldest_ts)
  assertU16('fw_version', s.fw_version)
  assertU8('batt', s.batt)
  const out = new Uint8Array(QUEUE_STATUS_SIZE)
  const dv = view(out)
  dv.setUint16(0, s.queued_count, true)
  dv.setUint32(2, s.oldest_ts, true)
  dv.setUint16(6, s.fw_version, true)
  dv.setUint8(8, s.batt)
  return out
}

export function decodeQueueStatus(bytes: Uint8Array): QueueStatus {
  if (bytes.byteLength !== QUEUE_STATUS_SIZE) {
    throw new RangeError(`QUEUE_STATUS must be ${QUEUE_STATUS_SIZE} bytes, got ${bytes.byteLength}`)
  }
  const dv = view(bytes)
  return {
    queued_count: dv.getUint16(0, true),
    oldest_ts: dv.getUint32(2, true),
    fw_version: dv.getUint16(6, true),
    batt: dv.getUint8(8),
  }
}

// ─────────────────────────────────────────────────────────────
// TIME_SYNC  (write) — fixed 4 bytes, little-endian
//
//   offset  size  field       type
//   0       4     epoch_utc   u32   seconds since Unix epoch (UTC)
//   ── total 4 bytes ──
// ─────────────────────────────────────────────────────────────

export interface TimeSync {
  epoch_utc: number
}

export const TIME_SYNC_SIZE = 4

export function encodeTimeSync(t: TimeSync): Uint8Array {
  assertU32('epoch_utc', t.epoch_utc)
  const out = new Uint8Array(TIME_SYNC_SIZE)
  view(out).setUint32(0, t.epoch_utc, true)
  return out
}

export function decodeTimeSync(bytes: Uint8Array): TimeSync {
  if (bytes.byteLength !== TIME_SYNC_SIZE) {
    throw new RangeError(`TIME_SYNC must be ${TIME_SYNC_SIZE} bytes, got ${bytes.byteLength}`)
  }
  return { epoch_utc: view(bytes).getUint32(0, true) }
}

// ─────────────────────────────────────────────────────────────
// RUN_ACK  (write) — fixed 38 bytes, little-endian ints
//
//   offset  size  field                  type    notes
//   0       16    local_id               uuid    watch's idempotency key
//   16      16    server_run_id          uuid    run.id assigned by Supabase
//   32      4     authoritative_xp       u32     child_progress.total_xp after trigger
//   36      2     authoritative_streak   u16     child_progress.current_streak after trigger
//   ── total 38 bytes ──
//
// The two 16-byte fields are UUIDs in canonical order (see header). This lets
// the watch match the ack to a queued run by local_id, then clear it and show
// the authoritative XP/streak.
// ─────────────────────────────────────────────────────────────

export interface RunAck {
  local_id: string
  server_run_id: string
  authoritative_xp: number
  authoritative_streak: number
}

export const RUN_ACK_SIZE = 38

export function encodeRunAck(a: RunAck): Uint8Array {
  assertU32('authoritative_xp', a.authoritative_xp)
  assertU16('authoritative_streak', a.authoritative_streak)
  const out = new Uint8Array(RUN_ACK_SIZE)
  out.set(uuidToBytes(a.local_id), 0)
  out.set(uuidToBytes(a.server_run_id), 16)
  const dv = view(out)
  dv.setUint32(32, a.authoritative_xp, true)
  dv.setUint16(36, a.authoritative_streak, true)
  return out
}

export function decodeRunAck(bytes: Uint8Array): RunAck {
  if (bytes.byteLength !== RUN_ACK_SIZE) {
    throw new RangeError(`RUN_ACK must be ${RUN_ACK_SIZE} bytes, got ${bytes.byteLength}`)
  }
  const dv = view(bytes)
  return {
    local_id: bytesToUuid(bytes.subarray(0, 16)),
    server_run_id: bytesToUuid(bytes.subarray(16, 32)),
    authoritative_xp: dv.getUint32(32, true),
    authoritative_streak: dv.getUint16(36, true),
  }
}

// ─────────────────────────────────────────────────────────────
// QUEUE_PULL  (read, multi-packet) — stream of length-prefixed run records
//
// Each record frame:
//   offset  size  field   type   notes
//   0       2     len     u16    byte length of the JSON that follows (LE)
//   2       len   json    utf8   JSON.stringify(WatchRunRecord)
//
// The stream is frame ‖ frame ‖ …  The watch emits one frame per queued run.
// decodeQueuePull tolerates a trailing PARTIAL frame (the last MTU packet may
// not have arrived yet) and reports how many bytes it consumed so the caller
// can retain the remainder and resume.
// ─────────────────────────────────────────────────────────────

const LEN16_MAX = 0xffff

export function encodeRunRecordFrame(record: WatchRunRecord): Uint8Array {
  const json = utf8Encode(JSON.stringify(record))
  if (json.byteLength > LEN16_MAX) {
    throw new RangeError(`run record JSON too large for u16 frame: ${json.byteLength} bytes`)
  }
  const out = new Uint8Array(2 + json.byteLength)
  view(out).setUint16(0, json.byteLength, true)
  out.set(json, 2)
  return out
}

export function encodeQueuePull(records: WatchRunRecord[]): Uint8Array {
  const frames = records.map(encodeRunRecordFrame)
  const total = frames.reduce((n, f) => n + f.byteLength, 0)
  const out = new Uint8Array(total)
  let off = 0
  for (const f of frames) { out.set(f, off); off += f.byteLength }
  return out
}

export interface QueuePullDecode {
  records: WatchRunRecord[]
  /** bytes consumed as complete frames; `bytes.byteLength - bytesConsumed` is a partial tail to retain. */
  bytesConsumed: number
}

export function decodeQueuePull(bytes: Uint8Array): QueuePullDecode {
  const records: WatchRunRecord[] = []
  const dv = view(bytes)
  let off = 0
  while (off + 2 <= bytes.byteLength) {
    const len = dv.getUint16(off, true)
    const end = off + 2 + len
    if (end > bytes.byteLength) break // partial trailing frame — stop, retain tail
    records.push(JSON.parse(utf8Decode(bytes.subarray(off + 2, end))) as WatchRunRecord)
    off = end
  }
  return { records, bytesConsumed: off }
}

// ─────────────────────────────────────────────────────────────
// ROUTINE_PUSH  (write, multi-packet) — one length-prefixed JSON blob
//
//   offset  size  field   type   notes
//   0       4     len     u32    byte length of the JSON that follows (LE)
//   4       len   json    utf8   the routines.json document
//
// u32 prefix (vs u16 for QUEUE_PULL) because a full routine set can exceed 64 KB.
// ─────────────────────────────────────────────────────────────

export function encodeRoutinePush(routinesJson: unknown): Uint8Array {
  const json = utf8Encode(typeof routinesJson === 'string' ? routinesJson : JSON.stringify(routinesJson))
  const out = new Uint8Array(4 + json.byteLength)
  view(out).setUint32(0, json.byteLength, true)
  out.set(json, 4)
  return out
}

export interface RoutinePushDecode<T = unknown> {
  routines: T
  bytesConsumed: number
}

/** Returns null if the full blob has not arrived yet (partial multi-packet write). */
export function decodeRoutinePush<T = unknown>(bytes: Uint8Array): RoutinePushDecode<T> | null {
  if (bytes.byteLength < 4) return null
  const len = view(bytes).getUint32(0, true)
  const end = 4 + len
  if (end > bytes.byteLength) return null // not fully reassembled yet
  return { routines: JSON.parse(utf8Decode(bytes.subarray(4, end))) as T, bytesConsumed: end }
}

// ─────────────────────────────────────────────────────────────
// RR_CONTROL  (write, multi-packet) — command envelope.  ADDED IN v2.
//
//   offset  size  field   type   notes
//   0       4     len     u32    byte length of the JSON that follows (LE)
//   4       len   json    utf8   { "cmd": "...", ...payload }
//
// Same framing as ROUTINE_PUSH (u32-prefixed JSON) so the watch reuses one
// reassembly path, and so a command can outgrow a single ATT write later.
// Commands today are well under 100 bytes and arrive in one packet.
//
// WHY THIS EXISTS: before v2, ROUTINE_PUSH carried routine data, the pairing
// nonce AND factory_reset — a command channel wearing a data-push name. Each
// new command made the envelope less honest. RR_CONTROL is the command
// channel; ROUTINE_PUSH went back to being routine data.
//
// Commands are additive: a watch that does not recognise a `cmd` rejects that
// one command and keeps working, so new commands do not need a version bump.
// ─────────────────────────────────────────────────────────────

export type RrControlCommand =
  /** Present the pairing nonce from the QR. Authenticates the CONNECTION for
   *  subsequent privileged writes (notably ROUTINE_PUSH). */
  | { cmd: 'nonce_auth'; nonce: string }
  /** Wipe pairing state, bonds and cache; the watch reboots to its QR. */
  | { cmd: 'factory_reset' }

export const RR_CONTROL_LEN_PREFIX_BYTES = 4

export function encodeControl(command: RrControlCommand): Uint8Array {
  const json = utf8Encode(JSON.stringify(command))
  const out = new Uint8Array(RR_CONTROL_LEN_PREFIX_BYTES + json.byteLength)
  view(out).setUint32(0, json.byteLength, true)
  out.set(json, RR_CONTROL_LEN_PREFIX_BYTES)
  return out
}

export interface ControlDecode {
  command: RrControlCommand
  bytesConsumed: number
}

/** Returns null if the full envelope has not arrived yet (multi-packet write). */
export function decodeControl(bytes: Uint8Array): ControlDecode | null {
  if (bytes.byteLength < RR_CONTROL_LEN_PREFIX_BYTES) return null
  const len = view(bytes).getUint32(0, true)
  const end = RR_CONTROL_LEN_PREFIX_BYTES + len
  if (end > bytes.byteLength) return null
  const command = JSON.parse(utf8Decode(bytes.subarray(RR_CONTROL_LEN_PREFIX_BYTES, end))) as RrControlCommand
  if (typeof (command as { cmd?: unknown })?.cmd !== 'string') {
    throw new TypeError('RR_CONTROL envelope has no "cmd" field')
  }
  return { command, bytesConsumed: end }
}

// ── UUID ↔ 16 bytes (canonical order) ────────────────────────────────────────

const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i

export function uuidToBytes(uuid: string): Uint8Array {
  if (!UUID_RE.test(uuid)) throw new TypeError(`not a canonical UUID: ${uuid}`)
  const hex = uuid.replace(/-/g, '')
  const out = new Uint8Array(16)
  for (let i = 0; i < 16; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16)
  return out
}

export function bytesToUuid(bytes: Uint8Array): string {
  if (bytes.byteLength !== 16) throw new RangeError(`UUID needs 16 bytes, got ${bytes.byteLength}`)
  const h = Array.from(bytes, b => b.toString(16).padStart(2, '0'))
  return `${h.slice(0, 4).join('')}-${h.slice(4, 6).join('')}-${h.slice(6, 8).join('')}-${h.slice(8, 10).join('')}-${h.slice(10, 16).join('')}`
}

// ── range guards (fail loud rather than silently wrap) ────────────────────────

function assertUint(field: string, v: number, max: number): void {
  if (!Number.isInteger(v) || v < 0 || v > max) {
    throw new RangeError(`${field} must be an integer in [0, ${max}], got ${v}`)
  }
}
const assertU8  = (f: string, v: number) => assertUint(f, v, 0xff)
const assertU16 = (f: string, v: number) => assertUint(f, v, 0xffff)
const assertU32 = (f: string, v: number) => assertUint(f, v, 0xffffffff)
