// ─────────────────────────────────────────────────────────────
// RR_SYNC — GATT contract for the Routine Rush watch companion
//
// CONTRACT VERSION 3 — re-frozen 2026-08-13.
//   v3 makes QUEUE_PULL PAGED: every read returns a 12-byte page header plus
//   at most 500 bytes, so a value can never exceed the 512-byte ATT ceiling.
//   A run record may now span pages, which removes the hard limit of ~2 steps
//   per routine that made every longer run permanently undrainable. Adds the
//   RR_CONTROL `queue_seek` command, which the reader uses to advance the read
//   cursor before every page — a QUEUE_PULL read is idempotent and moves
//   nothing (see CURSOR SEMANTICS; the first cut of v3 auto-advanced on the
//   watch and lost every other page on hardware).
//   No UUID changed; QUEUE_STATUS, RUN_ACK, TIME_SYNC, ROUTINE_PUSH and the
//   frame layer are untouched.
//
//   v2 adds RR_CONTROL (6th characteristic) and moves command traffic
//   (nonce_auth, factory_reset) off the ROUTINE_PUSH envelope onto it.
//   ROUTINE_PUSH is once again PURELY routine data.
//
//   Each of these was a deliberate, coordinated amendment — the freeze exists
//   to prevent SILENT drift, not to prevent evolution. Both sides regenerated.
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
  /** read (PAGED, v3) — 12-byte page header + ≤500 B of the framed run stream. */
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

// ═════════════════════════════════════════════════════════════
// QUEUE_PULL — PAGED. CHANGED IN v3.
//
// ── WHY v3 EXISTS ───────────────────────────────────────────
//
// 512 bytes is BLE_ATT_ATTR_MAX_LEN: the maximum length of an ATT attribute
// VALUE. It is a hard protocol ceiling, independent of MTU and independent of
// how either side fragments — a central simply cannot read more than that from
// one characteristic, ever.
//
// v2 served the whole unacked queue as one value and assumed it fit. A run
// record costs ~260 B fixed plus ~113 B per step, so:
//
//      2 steps ->  489 B framed   fits
//      3 steps ->  602 B framed   UNFETCHABLE
//      6 steps ->  941 B framed   UNFETCHABLE
//     11 steps -> 1506 B framed   UNFETCHABLE
//
// Past two steps the record could not be read, so it could never be acked, so
// it blocked every record behind it — permanently, and silently, because a
// blocked drain and an empty one looked identical. The v2 tests used a 2-step
// record, which is the ONLY size that fits, and that is why this shipped.
//
// Shrinking the record was considered and REJECTED: routines have no bounded
// step count, so it only moves the ceiling from 3 steps to ~15 and the bug
// returns in the field. Paging removes the ceiling instead of relocating it.
//
// ── THE TWO LAYERS ──────────────────────────────────────────
//
// PAGE layer (new in v3): each read returns a 12-byte header plus at most 500
// bytes of raw stream. A page NEVER exceeds 512 B by construction.
// FRAME layer (unchanged from v2): the concatenated pages form the same
// `[u16 len][json] ‖ [u16 len][json] ‖ …` stream v2 had.
//
// A FRAME MAY SPAN PAGES. That is the whole point — it is what lets a record of
// any size through. The reader concatenates page payloads and decodes whole
// frames out of the accumulated buffer, retaining a partial tail for the next
// page. decodeQueuePull already did exactly this (see `bytesConsumed`), so the
// frame layer needed no change at all.
//
// ── CURSOR SEMANTICS ────────────────────────────────────────
//
// `offset` is a byte offset into the UNACKED STREAM — the framed bytes starting
// at the queue's ack cursor. Not a file offset, not a record index.
//
//   • A QUEUE_PULL READ IS IDEMPOTENT. It serves the page at the watch's
//     current read cursor and does NOT move it. Reading twice without seeking
//     returns the same bytes twice.
//   • THE READER ADVANCES THE CURSOR, with RR_CONTROL `queue_seek`, before
//     every page. The phone's view of position is authoritative; the watch
//     never infers position from how many times it was read.
//   • Every page ECHOES the offset it starts at. The reader asserts this
//     matches what it seeked to, so a desync is caught at the page that causes
//     it rather than surfacing later as a corrupt frame.
//   • The watch RESETS the read offset to 0 on connect, on disconnect, and on
//     every accepted RUN_ACK. That only makes a forgotten seek start from a
//     record boundary — it is not a substitute for seeking.
//
// ⚠️ WHY THE WATCH MUST NOT AUTO-ADVANCE — THIS COST A HARDWARE DEBUG SESSION.
// v3 originally advanced the read cursor by `len` at the end of the read
// callback, so a drain was just repeated reads with no round trip per page. On
// real hardware the pages came out of the watch @0, @243, @486, @729 while the
// phone received @0 and then @486 — every other page, and the drain aborted on
// the offset gap (correctly).
//
// The cause is not fixable on the watch side: TWO ATT read callbacks can fire
// for ONE logical read. iOS may issue extra ATT_READ_REQ/ATT_READ_BLOB_REQ, and
// ble-plx's readCharacteristicForDevice exposes only the final value, so the
// central cannot even see it happened. Auto-advance means "position = how many
// times I was read", and a GATT server cannot know that number. Serving the
// page at the cursor makes a duplicated underlying read return the same bytes
// harmlessly — the duplicate becomes invisible instead of destructive.
//
// The wire LAYOUT is unchanged by this, so `version` stays 3: a page from a
// firmware that still auto-advances decodes fine and simply trips the reader's
// desync guard on page 2, which is a loud abort rather than corruption.
//
// ⚠️ ANY RUN_ACK INVALIDATES OUTSTANDING OFFSETS. An ack advances the ack
// cursor, so the unacked stream's base moves and every offset within it shifts.
// A reader must re-seek to 0 after acking — which is exactly what a drain pass
// does naturally. The watch's own reset makes this safe even if the reader
// forgets.
//
// Appends during a drain are safe and need no special handling: the log is
// append-only, so bytes already addressed keep their offsets and `total` simply
// grows.
//
// ── ACK IS PER RECORD, NEVER PER PAGE ───────────────────────
//
// A record that spans pages is acked ONCE, after the whole record has been
// reassembled AND relayed. Ack-before-advance is unchanged and is what makes a
// mid-drain drop safe:
//
//   Link drops halfway through a 3-page record -> nothing was acked -> the ack
//   cursor never moved -> on reconnect the reader seeks 0 and re-pulls that
//   record FROM ITS START. Nothing is lost (the record is still queued) and
//   nothing is duplicated (a partial record is never relayed, so the server
//   never saw it; and even a full re-relay dedupes on client_local_id).
//
// The alternative — acking each page — would let the cursor advance past bytes
// that never reached the server, which is the one loss the durable queue exists
// to prevent.
//
// ── PAGE HEADER ─────────────────────────────────────────────
//
// QUEUE_PULL_HEADER  (read) — fixed 12 bytes, little-endian
//
//   offset  size  field    type   notes
//   0       1     version  u8     = 3; a reader MUST reject anything else
//   1       1     flags    u8     bit0 = MORE (bytes remain after this page)
//   2       4     offset   u32    byte offset of this page in the unacked stream
//   6       4     total    u32    total bytes in the unacked stream right now
//   10      2     len      u16    payload bytes following this header
// ─────────────────────────────────────────────────────────────

/** The ATT ceiling on one attribute value. Not a tunable — see above. */
export const QUEUE_PULL_MAX_VALUE = 512
export const QUEUE_PULL_HEADER_SIZE = 12
/** Largest payload that keeps a whole page inside the ATT ceiling. */
export const QUEUE_PULL_MAX_PAYLOAD = QUEUE_PULL_MAX_VALUE - QUEUE_PULL_HEADER_SIZE // 500
export const QUEUE_PULL_VERSION = 3
/** flags bit0 — more bytes remain in the unacked stream after this page. */
export const QUEUE_PULL_FLAG_MORE = 0x01

export interface QueuePullPageWire {
  version: number
  more: boolean
  offset: number
  total: number
  payload: Uint8Array
}

export function encodeQueuePullPage(
  offset: number, total: number, payload: Uint8Array,
): Uint8Array {
  if (payload.byteLength > QUEUE_PULL_MAX_PAYLOAD) {
    throw new RangeError(
      `QUEUE_PULL page payload ${payload.byteLength} B exceeds ${QUEUE_PULL_MAX_PAYLOAD} B ` +
      `(would put the value over the ${QUEUE_PULL_MAX_VALUE} B ATT ceiling)`,
    )
  }
  const out = new Uint8Array(QUEUE_PULL_HEADER_SIZE + payload.byteLength)
  const dv = view(out)
  out[0] = QUEUE_PULL_VERSION
  out[1] = offset + payload.byteLength < total ? QUEUE_PULL_FLAG_MORE : 0
  dv.setUint32(2, offset, true)
  dv.setUint32(6, total, true)
  dv.setUint16(10, payload.byteLength, true)
  out.set(payload, QUEUE_PULL_HEADER_SIZE)
  return out
}

export function decodeQueuePullPage(bytes: Uint8Array): QueuePullPageWire {
  if (bytes.byteLength < QUEUE_PULL_HEADER_SIZE) {
    throw new RangeError(
      `QUEUE_PULL page must be at least ${QUEUE_PULL_HEADER_SIZE} bytes, got ${bytes.byteLength}`,
    )
  }
  if (bytes.byteLength > QUEUE_PULL_MAX_VALUE) {
    // A page over the ceiling means one side is not honouring the contract.
    // Fail here rather than let a silently truncated read become a corrupt frame.
    throw new RangeError(
      `QUEUE_PULL page is ${bytes.byteLength} B, over the ${QUEUE_PULL_MAX_VALUE} B ATT ceiling`,
    )
  }
  const dv = view(bytes)
  const version = bytes[0]
  if (version !== QUEUE_PULL_VERSION) {
    throw new RangeError(
      `QUEUE_PULL page version ${version}, expected ${QUEUE_PULL_VERSION} — ` +
      'the watch firmware and the app disagree on the contract',
    )
  }
  const len = dv.getUint16(10, true)
  const end = QUEUE_PULL_HEADER_SIZE + len
  if (end > bytes.byteLength) {
    throw new RangeError(
      `QUEUE_PULL page declares ${len} payload bytes but only ` +
      `${bytes.byteLength - QUEUE_PULL_HEADER_SIZE} arrived`,
    )
  }
  return {
    version,
    more: (bytes[1] & QUEUE_PULL_FLAG_MORE) !== 0,
    offset: dv.getUint32(2, true),
    total: dv.getUint32(6, true),
    payload: bytes.subarray(QUEUE_PULL_HEADER_SIZE, end),
  }
}

// ─────────────────────────────────────────────────────────────
// QUEUE_PULL frame layer — unchanged since v2.
//
// Each record frame:
//   offset  size  field   type   notes
//   0       2     len     u16    byte length of the JSON that follows (LE)
//   2       len   json    utf8   JSON.stringify(WatchRunRecord)
//
// The stream is frame ‖ frame ‖ …  The watch emits one frame per queued run.
// decodeQueuePull tolerates a trailing PARTIAL frame — in v2 that meant "the
// last MTU packet has not arrived"; in v3 it also means "this frame continues
// in the next page". It reports how many bytes it consumed so the caller can
// retain the remainder and resume.
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
//
// Commands so far:
//   nonce_auth     authorise this CONNECTION with the QR nonce   (v2)
//   factory_reset  wipe pairing, bonds, cache; reboot            (v2)
//   start_routine  start a routine on the watch, now, at step 1  (additive)
//   set_tz         UTC offset for local wall-clock display       (additive)
//   set_audio      speaker volume + quiet hours                  (additive)
//   queue_seek     set the QUEUE_PULL read offset                (v3)
// ─────────────────────────────────────────────────────────────

/**
 * `queue_seek` — set the byte offset the next QUEUE_PULL read serves from.
 *
 *   { "cmd": "queue_seek", "offset": <u32> }
 *
 * The offset is into the UNACKED STREAM (see the QUEUE_PULL section), so 0 is
 * always the first byte of the head unacked record — a record boundary, and
 * therefore always a safe place to restart.
 *
 * REQUIRED ON EVERY PAGE, not a recovery primitive. The watch serves the page
 * at its cursor and never moves it (see the QUEUE_PULL cursor semantics), so
 * this is the ONLY thing that advances a drain. A reader that seeks once and
 * then just reads gets page 1 forever.
 *
 * That is the point: paging position lives on the phone, which is the only side
 * that knows how many pages it has actually received. The watch cannot know —
 * one logical read can produce two ATT reads underneath.
 *
 * The two cases it also covers, for free:
 *
 *   • RESYNC. A page whose echoed `offset` is not what the reader seeked to
 *     means the two sides disagree about position. Seeking makes the reader's
 *     view authoritative instead of guessing.
 *   • RESTART. Beginning a drain, or resuming after a dropped link, seeks 0 so
 *     paging starts from a record boundary rather than wherever the previous
 *     attempt happened to stop.
 *
 * An offset beyond the end of the unacked stream is not an error: the next read
 * returns an empty page (len 0, MORE clear), which reads as "nothing there".
 *
 * Validated by encodeControl, like every other command — there is deliberately
 * no second encoder for it, so there is only one way to put it on the wire.
 */
export function assertQueueSeek(c: Extract<RrControlCommand, { cmd: 'queue_seek' }>): void {
  if (!Number.isInteger(c.offset) || c.offset < 0 || c.offset > 0xffffffff) {
    throw new RangeError(`queue_seek.offset must be a u32, got ${c.offset}`)
  }
}

export type RrControlCommand =
  /** Present the pairing nonce from the QR. Authenticates the CONNECTION for
   *  subsequent privileged writes (notably ROUTINE_PUSH). */
  | { cmd: 'nonce_auth'; nonce: string }
  /** Wipe pairing state, bonds and cache; the watch reboots to its QR. */
  | { cmd: 'factory_reset' }
  /** Start `routine_id` on the watch RIGHT NOW, at step 1 (remote start).
   *
   *  `routine_id` is the routine's `assignment_id` — the same id the watch
   *  caches from ROUTINE_PUSH, which in this repo is `routine.id` (there is no
   *  separate assignment table; see watchSync.ts).
   *
   *  Purely additive: a new `cmd` string on the existing envelope. No new
   *  characteristic, no byte-layout change — the v2 contract states outright
   *  that commands are additive and need no version bump. */
  | { cmd: 'start_routine'; routine_id: string }
  /** Set the watch's UTC offset for LOCAL WALL-CLOCK DISPLAY.
   *
   *  `offset_s` is total seconds to ADD to UTC to get local time, SIGNED so
   *  west-of-UTC works (Cyprus summer = +10800; New York winter = -18000).
   *
   *  WHY THIS IS A COMMAND AND NOT PART OF TIME_SYNC: TIME_SYNC is frozen at
   *  exactly 4 bytes — a bare u32 epoch — so there is physically nowhere in it
   *  to put an offset. Widening it would be a v3 layout change to a frozen
   *  characteristic. Riding RR_CONTROL is additive and costs nothing.
   *
   *  Send it on EVERY connect, alongside TIME_SYNC. An offset captured once
   *  goes stale at the next DST boundary; re-sending on every contact is what
   *  makes the watch self-correct without knowing any timezone rules. */
  | { cmd: 'set_tz'; offset_s: number }
  /** Set the speaker volume and the optional quiet-hours window.
   *
   *  WHY THIS EXISTS AT ALL: this board has NO VIBRATION MOTOR, so the watch's
   *  only way to get a child's attention is to make a noise. That makes volume
   *  a safety-ish setting rather than a preference — the same alarm that has to
   *  carry across a room at 07:00 must not go off at full volume in a bedroom
   *  at 21:30, and a parent is the only one who can judge that.
   *
   *  All fields are optional; omitted ones keep their current value, so the app
   *  can send a volume slider change without restating the quiet window.
   *
   *  `volume_pct`        0..100. 0 means visual-only (there is no haptic
   *                      fallback on this hardware — silent really is silent).
   *  `quiet_from` /      LOCAL "HH:MM", inclusive start / exclusive end. The
   *  `quiet_to`          window MAY WRAP MIDNIGHT and normally does
   *                      ("20:30" -> "06:30"). Send quiet_from: null to disable.
   *  `quiet_volume_pct`  the cap applied inside the window, 0..100. A cap, not
   *                      a replacement: outside the window the main volume
   *                      applies, and inside it the lower of the two wins.
   *
   *  Times are LOCAL because a parent sets them against a bedtime, not against
   *  UTC. The watch resolves them with the set_tz offset — the same rule the
   *  scheduler's "HH:MM" trigger times follow. */
  | {
      cmd: 'set_audio'
      volume_pct?: number
      quiet_from?: string | null
      quiet_to?: string
      quiet_volume_pct?: number
    }
  /** Set the QUEUE_PULL read offset. v3 — see encodeQueueSeek and the
   *  QUEUE_PULL section for cursor semantics. */
  | { cmd: 'queue_seek'; offset: number }

export const RR_CONTROL_LEN_PREFIX_BYTES = 4

/**
 * Bounds for `set_tz.offset_s`. Real UTC offsets span UTC-12:00 to UTC+14:00
 * (Kiribati), and there are :45 zones (Nepal +05:45, Chatham +12:45), so the
 * range is checked but the granularity is not.
 */
export const TZ_OFFSET_MIN_S = -12 * 3600
export const TZ_OFFSET_MAX_S = 14 * 3600

/**
 * Bounds for `set_audio`. Percentages are 0..100 inclusive; 0 is a legal and
 * meaningful value (visual-only), not "unset".
 */
export const AUDIO_VOLUME_MIN_PCT = 0
export const AUDIO_VOLUME_MAX_PCT = 100

/** `quiet_from` / `quiet_to` are LOCAL 24-hour "HH:MM". */
export const AUDIO_QUIET_TIME_RE = /^([01]\d|2[0-3]):([0-5]\d)$/

/**
 * Validate a set_audio command before it goes on the wire.
 *
 * The firmware rejects a bad field with ATT 0x0E and applies NOTHING (see
 * rr_ble.c set_audio), so a malformed send is a silent no-op from the parent's
 * point of view. Failing here instead turns that into a real error with a
 * reason, which is the difference between "the slider did nothing" and a bug
 * report someone can act on.
 */
export function assertSetAudio(c: Extract<RrControlCommand, { cmd: 'set_audio' }>): void {
  const pct = (name: string, v: number | undefined) => {
    if (v === undefined) return
    if (!Number.isInteger(v) || v < AUDIO_VOLUME_MIN_PCT || v > AUDIO_VOLUME_MAX_PCT) {
      throw new RangeError(`set_audio.${name} must be an integer 0..100, got ${v}`)
    }
  }
  pct('volume_pct', c.volume_pct)
  pct('quiet_volume_pct', c.quiet_volume_pct)

  // null is meaningful — it DISABLES the window — and is distinct from omitting
  // the field, which leaves it alone. Only a string has to look like a time.
  if (typeof c.quiet_from === 'string' && !AUDIO_QUIET_TIME_RE.test(c.quiet_from)) {
    throw new RangeError(`set_audio.quiet_from must be LOCAL "HH:MM", got ${c.quiet_from}`)
  }
  if (c.quiet_to !== undefined && !AUDIO_QUIET_TIME_RE.test(c.quiet_to)) {
    throw new RangeError(`set_audio.quiet_to must be LOCAL "HH:MM", got ${c.quiet_to}`)
  }
  // A window needs both ends. Sending only quiet_to would silently pair a new
  // end with whatever start the watch already had.
  if (typeof c.quiet_from === 'string' && c.quiet_to === undefined) {
    throw new RangeError('set_audio.quiet_from given without quiet_to')
  }
}

// ── RR_CONTROL response codes ────────────────────────────────────────────────
//
// RR_CONTROL is write-only with NO notify, by design: "the ATT write response
// already carries success or failure". So an outcome that is richer than
// yes/no has to ride the ATT error code, which is exactly what the contract
// intends — 0x05 already means "not authorised" and 0x06 already means
// "unknown command".
//
// `start_routine` needs two more outcomes than those, so it uses the ATT
// APPLICATION error range (0x80–0x9F), which the Bluetooth core spec reserves
// for profile-defined errors. This is a semantic addition to an existing
// channel, NOT a layout change: no characteristic is added and no byte moves.
//
// Codes are per-command, not global — a future command may reuse 0x80 for its
// own "the state is wrong for this" meaning, since the phone always knows
// which command it just wrote.
export const RR_CONTROL_ATT = {
  /** Success. The write response carries no error at all. */
  OK: 0,
  /** 0x05 — the link is not the paired peer (or is unencrypted). */
  UNAUTHORISED: 0x05,
  /** 0x06 — this firmware does not know the command. */
  UNKNOWN_COMMAND: 0x06,
  /** 0x80 — `start_routine`: a routine is already running; refused. */
  BUSY: 0x80,
  /** 0x81 — `start_routine`: that routine_id is not in the watch's cache. */
  UNKNOWN_ROUTINE: 0x81,
} as const

export function encodeControl(command: RrControlCommand): Uint8Array {
  if (command.cmd === 'set_audio') assertSetAudio(command)
  if (command.cmd === 'queue_seek') assertQueueSeek(command)
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
