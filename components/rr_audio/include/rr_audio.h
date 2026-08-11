#pragma once
// rr_audio — ES8311 effect/jingle playback. Phase 8.
//
// ON THE CRITICAL PATH. This board has NO VIBRATION MOTOR (Phase 0 finding),
// so audio is the only alerting channel — a scheduler that fires silently is a
// routine nobody notices. Phase 7 (scheduler) depends on this module.
//
// Verified working on hardware in Phase 0:
//   ES8311 at I2C 0x18, "Work in Slave mode"
//   I2S TX, 16-bit mono @ 22.05 kHz (BSP default via bsp_audio_init(NULL))
//   Power-amp enable is GPIO 6 — driven automatically by esp_codec_dev on
//   open/close. DO NOT toggle GPIO 6 from this module.
//
// Init sequence that is known to produce audible output:
//   bsp_audio_init(NULL);
//   esp_codec_dev_handle_t spk = bsp_audio_codec_speaker_init();
//   esp_codec_dev_open(spk, &fs);  esp_codec_dev_set_out_vol(spk, vol);
//   esp_codec_dev_write(spk, pcm, bytes);  esp_codec_dev_close(spk);
//
// ⚠️ No vendor example plays audio OUT — Waveshare's 05_Spec_Analyzer is
// microphone capture only. There is no reference implementation to crib from;
// the sequence above is the starting point.
//
// Short pre-baked WAV/ADPCM clips only. No MP3 decoder in the build.
//
// Intended interface:
//
//   esp_err_t rr_audio_init(void);
//   esp_err_t rr_audio_play_effect(rr_effect_t id);   // non-blocking
//   esp_err_t rr_audio_play_voice(uint16_t prompt_id);
//   void      rr_audio_stop(void);
//   void      rr_audio_set_volume(uint8_t pct);       // 0..100, hardware vol
//
// Because the alert is AUDIBLE (not a silent buzz), volume and quiet hours are
// product requirements, not polish: keep volume parent-controlled and cap it
// at night. "Silent mode" on this board can only mean visual-only.
