#pragma once
// rr_audio — ES8311 playback.
//
// ON THE CRITICAL PATH. This board has NO VIBRATION MOTOR (Phase 0 finding),
// so audio is the only alerting channel — a scheduler that fires silently is a
// routine nobody notices. Phase 7 (scheduler) depends on this module.
//
// ── SCOPE: what Phase 7 built, and what Phase 8 still owes ──────────────────
//
// Phase 8 owns the full audio subsystem (§10B): voice prompts per step, the
// step-done blip, the XP chime, the completion fanfare, ducking, a mixer.
// NONE of that is here. Phase 7 needed exactly one thing — the alarm must be
// audible — so this is the minimum that makes a sound and no more:
//
//   • one clip at a time, played to completion (no mixing, no ducking)
//   • a one-deep request queue: a new request while a clip plays REPLACES the
//     queued one rather than stacking, because an alarm ringing twice over
//     itself is worse than an alarm ringing once
//   • WAV PCM s16le only, at the codec's native rate — no decode, no resample
//
// The voice-prompt table in assets/audio/audio_manifest.h is already generated
// and shipped in the littlefs image; rr_audio_play_file() will play those clips
// today. Wiring them into the step runtime is Phase 8's job, not this module's.
//
// Verified working on hardware in Phase 0:
//   ES8311 at I2C 0x18, "Work in Slave mode"
//   I2S TX, 16-bit mono @ 22.05 kHz (BSP default via bsp_audio_init(NULL))
//   Power-amp enable is GPIO 6 — driven automatically by esp_codec_dev on
//   open/close. DO NOT toggle GPIO 6 from this module.
//
// ⚠️ No vendor example plays audio OUT — Waveshare's 05_Spec_Analyzer is
// microphone capture only. There is no reference implementation to crib from.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "audio_manifest.h"   // generated: rr_tone_id_t, RR_TONE_TABLE, paths

/**
 * Bring up I2S + the ES8311 and start the player task.
 *
 * Safe to call on a watch whose littlefs has no audio in it: init succeeds and
 * playback requests fail loudly per-clip. That distinction matters because a
 * firmware flash without the asset image is the most likely reason for a
 * silent alarm, and it should read as "no clip" and not "no codec".
 */
esp_err_t rr_audio_init(void);

/** False if init failed — callers use it to say WHY they are about to be silent. */
bool rr_audio_is_ready(void);

/**
 * Queue one of the generated tones (RR_TONE_ALARM, ...). Returns as soon as
 * the request is queued — SAFE FROM ANY TASK, and specifically safe from the
 * scheduler, which must not block on ~2.5 s of audio before starting a routine.
 */
esp_err_t rr_audio_play_tone(rr_tone_id_t id);

/** Queue an arbitrary WAV from littlefs (the voice-prompt paths). */
esp_err_t rr_audio_play_file(const char *path);

/** Stop the clip in flight, if any. */
void rr_audio_stop(void);

/**
 * Output volume, 0..100, applied to the codec and used for every later clip.
 *
 * §7 calls for a night-time cap and a visual-only "silent" mode, both of which
 * belong to the parent app and neither of which has a wire format yet — there
 * is no volume field in the v2 contract and the contract is frozen. Until one
 * exists this is a firmware-local default, and 0 is honoured as "do not play"
 * so the plumbing is ready for it.
 */
void rr_audio_set_volume(uint8_t pct);
uint8_t rr_audio_get_volume(void);
