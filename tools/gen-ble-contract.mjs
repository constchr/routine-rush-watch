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
// QUEUE_PULL:    [u16 len][len bytes UTF-8 JSON]  repeated, one frame per run.
//                A trailing PARTIAL frame is legal — retain the tail and resume.
// ROUTINE_PUSH:  [u32 len][len bytes UTF-8 JSON]  one blob (u32 because a full
//                routine set can exceed 64 KB). v2: PURE routine data.
// RR_CONTROL:    [u32 len][len bytes UTF-8 JSON]  one command envelope,
//                { "cmd": "...", ...payload }. Added in contract v2.
#define RR_QUEUE_PULL_LEN_PREFIX_BYTES   2
#define RR_ROUTINE_PUSH_LEN_PREFIX_BYTES 4
#define RR_CONTROL_LEN_PREFIX_BYTES      ${controlPrefix}
`

if (process.argv.includes('--check')) {
  let current = ''
  try { current = readFileSync(OUT, 'utf8') } catch { /* not generated yet */ }
  if (current !== h) {
    fail('ble_contract.h is stale or missing — run: node tools/gen-ble-contract.mjs')
  }
  console.log('gen-ble-contract: header is up to date')
  process.exit(0)
}

mkdirSync(dirname(OUT), { recursive: true })
writeFileSync(OUT, h)
console.log(`gen-ble-contract: wrote ${OUT}`)
console.log(`  service ${serviceUuid}`)
console.log(`  ${chars.length} characteristics, ${Object.keys(layouts).length} fixed layouts validated`)
