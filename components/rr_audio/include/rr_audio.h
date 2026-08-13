#pragma once
// rr_audio — ES8311 playback. Phase 8.
//
// ON THE CRITICAL PATH. This board has NO VIBRATION MOTOR (Phase 0 finding),
// so audio is the only alerting channel — a scheduler that fires silently is a
// routine nobody notices.
//
// Verified working on hardware in Phase 0/7:
//   ES8311 at I2C 0x18, "Work in Slave mode"
//   I2S TX, 16-bit mono @ 22.05 kHz (BSP default via bsp_audio_init(NULL))
//   Power-amp enable is GPIO 6 — driven automatically by esp_codec_dev on
//   open/close. DO NOT toggle GPIO 6 from this module.
//
// ── ONE ENCODING, ONE PATH ──────────────────────────────────────────────────
//
// Every clip is WAV PCM s16le at the BSP's native rate and streams straight to
// the I2S DMA ring. There is no decoder and no second format.
//
// It was not always so: Phase 8 added an IMA ADPCM path for 36 pre-rendered
// voice prompts, because the 16-bit voice set was 2.5 MB against a partition
// already holding 4.1 MB of emoji. Those prompts are gone — `template_step.label`
// is free text a parent types, so pre-rendered speech only ever covered the 16
// hardcoded starter templates and the common case was a lookup miss falling back
// to a tone. Removing them returned ~625 KB of littlefs and left the playback
// path that was verified on hardware as the only one.
//
// ── ORDERING ────────────────────────────────────────────────────────────────
//
// One clip at a time, no mixing. Effects QUEUE (so a completion fanfare can be
// followed by an XP chime), but the alarm PREEMPTS — it flushes whatever is
// pending and plays now, because an alarm that waits behind a step blip is an
// alarm that rings late.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#include "audio_manifest.h"   // generated: rr_tone_id_t + the tone table

/** Bring up I2S + the ES8311 and start the player task. */
esp_err_t rr_audio_init(void);

/** False if init failed — callers use it to say WHY they are about to be silent. */
bool rr_audio_is_ready(void);

/** Queue one of the generated tones. Non-blocking, safe from any task. */
esp_err_t rr_audio_play_tone(rr_tone_id_t id);

/** Queue an arbitrary clip from littlefs. */
esp_err_t rr_audio_play_file(const char *path);

/**
 * The alarm: flush anything queued, stop anything playing, ring NOW.
 *
 * Separate from rr_audio_play_tone(RR_TONE_ALARM) on purpose — the queue is
 * FIFO for effects, and the one sound on this watch that must never wait in
 * line is the one that substitutes for a vibration motor.
 */
esp_err_t rr_audio_play_alarm(void);

/** Stop the clip in flight and drop anything queued behind it. */
void rr_audio_stop(void);

// ── Volume + quiet hours (§7, §10B.4) ───────────────────────────────────────
//
// Because the alert is AUDIBLE rather than a silent buzz, volume is a product
// requirement and not polish: the same alarm that has to carry across a room at
// 07:00 must not blast a bedroom at 21:30. Parents own this through the
// RR_CONTROL `set_audio` command; the values persist in NVS so a watch on a
// wrist with no phone nearby keeps them.

#define RR_AUDIO_QUIET_DISABLED (-1)

typedef struct {
    uint8_t volume_pct;        /**< 0..100. 0 means visual-only. */
    int16_t quiet_from_min;    /**< minutes past LOCAL midnight, or RR_AUDIO_QUIET_DISABLED */
    int16_t quiet_to_min;      /**< exclusive; may wrap past midnight (21:00 -> 06:30) */
    uint8_t quiet_volume_pct;  /**< cap applied inside the window. 0 = silent. */
} rr_audio_policy_t;

/** Apply and persist. Values are range-checked; a bad one is rejected whole. */
esp_err_t rr_audio_set_policy(const rr_audio_policy_t *p);

void rr_audio_get_policy(rr_audio_policy_t *out);

/** True if LOCAL wall-clock is inside the quiet window right now. */
bool rr_audio_in_quiet_hours(void);

/** The volume the next clip will actually use, after the quiet-hours cap. */
uint8_t rr_audio_effective_volume(void);
