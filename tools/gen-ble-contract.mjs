#!/usr/bin/env node
// Generates components/rr_ble/include/ble_contract.h from watchProtocol.ts.
//
// THE APP REPO IS THE SOURCE OF TRUTH. tools/watchProtocol.ts here is a
// vendored copy of routine-rush/packages/ble/src/watchProtocol.ts (spec §6B.3,
// FROZEN CONTRACT). Never edit either the vendored copy or the generated
// header by hand — change the app repo, re-vendor, regenerate.
//
// CI runs `--check` to fail if the committed vendored copy has drifted from
// the app repo, so the two ends of the protocol cannot silently disagree.
//
// Usage:
//   node tools/gen-ble-contract.mjs            # write the header
//   node tools/gen-ble-contract.mjs --check    # verify only, non-zero on drift

import { readFileSync, writeFileSync, mkdirSync } from 'node:fs'
import { dirname, join } from 'node:path'
import { fileURLToPath } from 'node:url'
import { createHash } from 'node:crypto'

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..')
const SRC = join(ROOT, 'tools', 'watchProtocol.ts')
const OUT = join(ROOT, 'components', 'rr_ble', 'include', 'ble_contract.h')

const src = readFileSync(SRC, 'utf8')
const sha = createHash('sha256').update(src).digest('hex').slice(0, 16)

const fail = (msg) => {
  console.error(`gen-ble-contract: ${msg}`)
  process.exit(1)
}

// ── UUIDs — parsed from real code, not comments ─────────────────────────────
const serviceUuid = src.match(
  /RR_SYNC_SERVICE_UUID\s*=\s*'([0-9a-f-]{36})'/i,
)?.[1] ?? fail('could not find RR_SYNC_SERVICE_UUID')

const charBlock = src.match(/RR_SYNC_CHARACTERISTICS\s*=\s*\{([\s\S]*?)\}\s*as const/)?.[1]
  ?? fail('could not find RR_SYNC_CHARACTERISTICS')

const chars = [...charBlock.matchAll(/(\w+)\s*:\s*'([0-9a-f-]{36})'/gi)]
  .map(([, name, uuid]) => ({ name, uuid }))
if (chars.length === 0) fail('parsed zero characteristics')

// ── Exported size constants ─────────────────────────────────────────────────
const sizes = Object.fromEntries(
  [...src.matchAll(/export const (\w+_SIZE)\s*=\s*(\d+)/g)].map(([, k, v]) => [k, Number(v)]),
)

// ── Field tables from the structured comment blocks ─────────────────────────
// Each fixed-layout message documents itself as an aligned table:
//     //   offset  size  field          type   notes
//     //   0       2     queued_count   u16    ...
// We parse that, then CHECK it against the exported *_SIZE constant. A
// mismatch means the comment and the code disagree — refuse to emit rather
// than bake a wrong offset into the firmware.
const TYPES = { u8: 'uint8_t', u16: 'uint16_t', u32: 'uint32_t', uuid: 'uint8_t' }

function parseLayout(msgName) {
  const block = src.match(new RegExp(`//\\s*${msgName}\\s*\\(([\\s\\S]*?)// ─────`, 'm'))?.[1]
  if (!block) fail(`no layout comment block for ${msgName}`)
  const fields = [...block.matchAll(/^\/\/\s+(\d+)\s+(\d+)\s+(\w+)\s+(u8|u16|u32|uuid)\b/gm)]
    .map(([, off, size, name, type]) => ({
      offset: Number(off), size: Number(size), name, type,
    }))
  if (fields.length === 0) fail(`parsed zero fields for ${msgName}`)

  // Contiguity + total-size check.
  let expected = 0
  for (const f of fields) {
    if (f.offset !== expected) {
      fail(`${msgName}.${f.name}: offset ${f.offset} but previous fields end at ${expected} — layout table is inconsistent`)
    }
    expected += f.size
  }
  const declared = sizes[`${msgName}_SIZE`]
  if (declared === undefined) fail(`no exported ${msgName}_SIZE to validate against`)
  if (declared !== expected) {
    fail(`${msgName}: fields sum to ${expected} bytes but ${msgName}_SIZE is ${declared} — contract is self-inconsistent`)
  }
  return { fields, size: declared }
}

// Framing widths come from the contract, not from this generator, so a change
// there cannot silently disagree with the firmware.
const controlPrefix = Number(
  src.match(/export const RR_CONTROL_LEN_PREFIX_BYTES\s*=\s*(\d+)/)?.[1]
  ?? fail('could not find RR_CONTROL_LEN_PREFIX_BYTES in the contract'),
)

const layouts = {
  QUEUE_STATUS: parseLayout('QUEUE_STATUS'),
  TIME_SYNC: parseLayout('TIME_SYNC'),
  RUN_ACK: parseLayout('RUN_ACK'),
  // v3: QUEUE_PULL's page header is a fixed layout like any other, so it gets
  // the same offset/size validation and the same _Static_assert on the C side.
  QUEUE_PULL_HEADER: parseLayout('QUEUE_PULL_HEADER'),
}

// ── v3 paging constants ─────────────────────────────────────────────────────
// Parsed, never retyped. If the firmware and the contract disagreed about the
// 512-byte ceiling or the 12-byte header, the result would be a value that
// overflows an ATT read — which is the exact bug v3 exists to remove, so it is
// the last thing that should be duplicated by hand.
const num = (name) => Number(
  src.match(new RegExp(`export const ${name}\\s*=\\s*(0x[0-9a-f]+|\\d+)`, 'i'))?.[1]
  ?? fail(`could not find ${name} in the contract`),
)
const pullMaxValue = num('QUEUE_PULL_MAX_VALUE')
const pullHeaderSize = num('QUEUE_PULL_HEADER_SIZE')
const pullVersion = num('QUEUE_PULL_VERSION')
const pullFlagMore = num('QUEUE_PULL_FLAG_MORE')
const pullMaxPayload = pullMaxValue - pullHeaderSize

if (pullHeaderSize !== layouts.QUEUE_PULL_HEADER.size) {
  fail(`QUEUE_PULL_MAX_VALUE/HEADER_SIZE disagree with the parsed header layout `
       + `(${pullHeaderSize} vs ${layouts.QUEUE_PULL_HEADER.size})`)
}
if (pullMaxPayload <= 0) fail('QUEUE_PULL header does not leave room for a payload')

// ── RR_CONTROL response codes ───────────────────────────────────────────────
// The ATT statuses a command handler returns. They ARE contract — the phone
// switches on them to tell a parent "the watch is busy" rather than "something
// went wrong" — so they are generated, not retyped into the firmware where
// they could drift by one hex digit and be wrong in a way nothing catches.
const attBlock = src.match(/export const RR_CONTROL_ATT\s*=\s*\{([\s\S]*?)\}\s*as const/)?.[1]
  ?? fail('could not find RR_CONTROL_ATT in the contract')
const attCodes = [...attBlock.matchAll(/^\s*(\w+)\s*:\s*(0x[0-9a-f]+|\d+)\s*,/gim)]
  .map(([, name, value]) => ({ name, value: Number(value) }))
if (attCodes.length === 0) fail('parsed zero RR_CONTROL_ATT codes')
for (const c of attCodes) {
  if (!Number.isInteger(c.value) || c.value < 0 || c.value > 0xff) {
    fail(`RR_CONTROL_ATT.${c.name} = ${c.value} is not a byte-sized ATT status`)
  }
}

// ── Emit ────────────────────────────────────────────────────────────────────
const uuidToCArray = (u) =>
  '{ ' + u.replace(/-/g, '').match(/../g).map((b) => `0x${b}`).join(', ') + ' }'

let h = `// ─────────────────────────────────────────────────────────────────────────────
// ble_contract.h — RR_SYNC GATT contract.  GENERATED FILE. DO NOT EDIT.
//
//   source:    tools/watchProtocol.ts  (sha256:${sha})
//   generator: tools/gen-ble-contract.mjs
//   regenerate: node tools/gen-ble-contract.mjs
//
// The app repo (routine-rush/packages/ble/src/watchProtocol.ts) is the SOURCE
// OF TRUTH — spec §6B.3, FROZEN CONTRACT. The phone and this firmware
// implement opposite ends of these exact byte layouts.
//
// Conventions carried over from the contract:
//   • All multi-byte integers are LITTLE-ENDIAN.
//   • UUID fields are 16 bytes in CANONICAL order (as read left-to-right),
//     NOT endian-swapped.
//   • Variable-length payloads are length-prefixed UTF-8 JSON.
//
// Endianness note: the contract comments describe the watch as "ARM Cortex-M".
// This board is RISC-V (ESP32-C6). Both are little-endian, so every layout
// below is still correct and structs may be memcpy'd directly — only the
// comment in the app repo is inaccurate.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <stdint.h>

// ── Service ──────────────────────────────────────────────────────────────────
#define RR_SYNC_SERVICE_UUID_STR "${serviceUuid}"
#define RR_SYNC_SERVICE_UUID_BYTES ${uuidToCArray(serviceUuid)}

// ── Characteristics ──────────────────────────────────────────────────────────
`

for (const c of chars) {
  h += `#define RR_SYNC_CHAR_${c.name}_UUID_STR   "${c.uuid}"\n`
  h += `#define RR_SYNC_CHAR_${c.name}_UUID_BYTES ${uuidToCArray(c.uuid)}\n`
}

h += `
// ── Fixed-layout messages ────────────────────────────────────────────────────
// Offsets validated at generation time against the exported *_SIZE constants.
`

for (const [name, { fields, size }] of Object.entries(layouts)) {
  h += `\n#define RR_${name}_SIZE ${size}\n`
  h += `typedef struct __attribute__((packed)) {\n`
  for (const f of fields) {
    const cType = TYPES[f.type]
    const decl = f.type === 'uuid' ? `${cType} ${f.name}[16];` : `${cType} ${f.name};`
    h += `    ${decl.padEnd(38)} // offset ${String(f.offset).padStart(2)}, ${f.size} B, ${f.type}\n`
  }
  h += `} rr_${name.toLowerCase()}_t;\n`
  h += `_Static_assert(sizeof(rr_${name.toLowerCase()}_t) == RR_${name}_SIZE,\n`
  h += `               "rr_${name.toLowerCase()}_t must be exactly ${size} bytes — check packing");\n`
}

h += `
// ── Variable-length framing ──────────────────────────────────────────────────
// QUEUE_PULL:    PAGED since v3. Every read is [12-byte page header][payload],
//                payload <= ${pullMaxPayload} B, so the value can never exceed the
//                ${pullMaxValue}-byte ATT ceiling (BLE_ATT_ATTR_MAX_LEN).
//                The concatenated payloads form the frame stream:
//                  [u16 len][len bytes UTF-8 JSON]  repeated, one frame per run.
//                A FRAME MAY SPAN PAGES — that is what lets a run record of any
//                step count through. A trailing PARTIAL frame is legal; retain
//                the tail and resume with the next page.
// ROUTINE_PUSH:  [u32 len][len bytes UTF-8 JSON]  one blob (u32 because a full
//                routine set can exceed 64 KB). v2: PURE routine data.
// RR_CONTROL:    [u32 len][len bytes UTF-8 JSON]  one command envelope,
//                { "cmd": "...", ...payload }. Added in contract v2.
#define RR_QUEUE_PULL_LEN_PREFIX_BYTES   2
#define RR_ROUTINE_PUSH_LEN_PREFIX_BYTES 4
#define RR_CONTROL_LEN_PREFIX_BYTES      ${controlPrefix}

// ── QUEUE_PULL paging (contract v3) ──────────────────────────────────────────
// RR_QUEUE_PULL_MAX_VALUE is a HARD BLUETOOTH CEILING, not a tunable: it is the
// maximum length of an ATT attribute value, so a central cannot read more than
// this from one characteristic whatever the MTU is. Serving more silently
// produces a value the phone can never fully read — which is what made every
// run record over ~2 steps permanently undrainable before v3.
// (RR_QUEUE_PULL_HEADER_SIZE is emitted above with the header's struct layout.)
#define RR_QUEUE_PULL_VERSION            ${pullVersion}
#define RR_QUEUE_PULL_MAX_VALUE          ${pullMaxValue}
#define RR_QUEUE_PULL_MAX_PAYLOAD        ${pullMaxPayload}
#define RR_QUEUE_PULL_FLAG_MORE          0x${pullFlagMore.toString(16).padStart(2, '0')}
_Static_assert(RR_QUEUE_PULL_HEADER_SIZE + RR_QUEUE_PULL_MAX_PAYLOAD
               <= RR_QUEUE_PULL_MAX_VALUE,
               "a QUEUE_PULL page must fit inside the ATT attribute ceiling");

// ── RR_CONTROL response codes ────────────────────────────────────────────────
// RR_CONTROL is write-only with NO notify: the ATT write response IS the reply
// channel (§6B.3). A command handler returns one of these from its access
// callback and NimBLE puts it on the wire as the ATT status.
//
// 0x00 is success; 0x05/0x06 are standard ATT errors the contract already
// assigns meaning to; 0x80+ are in the ATT APPLICATION range (0x80-0x9F) that
// the core spec reserves for profile-defined errors, and are scoped PER
// COMMAND — the phone always knows which command it just wrote.
${attCodes.map(c =>
  `#define RR_CONTROL_ATT_${c.name.padEnd(16)} 0x${c.value.toString(16).padStart(2, '0')}`,
).join('\n')}
`

if (process.argv.includes('--check')) {
  // ── ABSENT IS NOT DRIFT ─────────────────────────────────────────────────
  //
  // This used to be `if (current !== h) fail(...)`, which conflated two
  // completely different conditions: a header that DISAGREES with the contract
  // (a real problem — the firmware would be built against a stale protocol) and
  // a header that simply is not there yet.
  //
  // ble_contract.h is a BUILD ARTEFACT and is gitignored (.gitignore), so on a
  // clean checkout it can never exist — which meant --check failed by
  // construction in CI. That is exactly what happened on this repo's first-ever
  // Actions run (b797d33): the "Header matches vendored contract" step exited 1,
  // and because it failed, the step that actually matters — the cross-repo drift
  // check against the app repo — was SKIPPED and never evaluated. A spurious
  // failure hid a real one.
  //
  // Note what --check still protects, regardless of the header: every validation
  // in this file (layout offsets summing to the exported *_SIZE constants, ATT
  // codes being byte-sized, the characteristic table parsing at all) runs above
  // this point and calls fail() on its own. Reaching here already means the
  // contract is self-consistent. The header comparison is the smaller half.
  //
  // Fixed script-side rather than by generating before checking in the workflow:
  // "generate, then compare the generated file to itself" always passes, so it
  // would have turned the step into a no-op that still LOOKED like a check —
  // and it would have left `--check` misleading for anyone running it locally
  // after switching branches, which is the case it is genuinely useful for.
  let current = null
  try { current = readFileSync(OUT, 'utf8') } catch { /* absent — handled below */ }

  if (current === null) {
    // Produce it, so the caller ends up in the state they were checking for.
    mkdirSync(dirname(OUT), { recursive: true })
    writeFileSync(OUT, h)
    console.log('gen-ble-contract: contract is self-consistent; header was absent '
                + '(build artefact, gitignored) and has been generated')
    process.exit(0)
  }

  if (current !== h) {
    fail('ble_contract.h DISAGREES with tools/watchProtocol.ts — regenerate: '
         + 'node tools/gen-ble-contract.mjs')
  }

  console.log('gen-ble-contract: header is up to date')
  process.exit(0)
}

mkdirSync(dirname(OUT), { recursive: true })
writeFileSync(OUT, h)
console.log(`gen-ble-contract: wrote ${OUT}`)
console.log(`  service ${serviceUuid}`)
console.log(`  ${chars.length} characteristics, ${Object.keys(layouts).length} fixed layouts validated`)
