// rr_audio — ES8311 playback: PCM tones, ADPCM voice prompts, volume policy.
// See rr_audio.h for the encoding split, the ordering rules and why they exist.

#include "rr_audio.h"

#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"

#include "bsp/esp-bsp.h"
#include "driver/i2s_std.h"
#include "es8311_codec.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"

#include "rr_rtc.h"

static const char *TAG = "rr_audio";

// PCM is streamed in chunks rather than loaded whole: tone_alarm.wav is ~107 KiB
// and the free heap here is ~160 KiB. Loading a clip would spend most of a
// second's worth of RAM to play a two-second sound.
#define PCM_CHUNK_BYTES 2048

// One ADPCM block decodes to RR_AUDIO_ADPCM_SAMPLES_PER_BLOCK 16-bit samples.
// Both buffers are allocated once for the life of the player task rather than
// per clip, so playback never fails for want of a 2 KB allocation.
#define ADPCM_PCM_BYTES (RR_AUDIO_ADPCM_SAMPLES_PER_BLOCK * 2)

#define PLAYER_STACK 4096
#define PLAYER_PRIO  4          /* below LVGL: a late tone beats a torn frame */

// Effects queue rather than clobber, so complete-fanfare then XP-chime plays as
// two sounds instead of one. Three is enough for the longest real sequence.
#define REQUEST_DEPTH 3

#define NVS_NAMESPACE "rr_audio"
#define NVS_KEY_POLICY "policy"

// ── Defaults ────────────────────────────────────────────────────────────────
//
// Quiet hours default ON. There is no parent UI for this yet, and of the two
// possible wrong defaults — "a bedroom gets a full-volume alarm at 21:30" and
// "a morning alarm is quieter than it could be" — only one of them wakes a
// household. The window ends at 06:30 so a school-morning routine still rings
// at full volume, and the cap is a reduction rather than a mute so a bedtime
// routine is still noticeable. Parents override all of it via `set_audio`.
#define DEFAULT_VOLUME_PCT       70
#define DEFAULT_QUIET_FROM_MIN   (20 * 60 + 30)
#define DEFAULT_QUIET_TO_MIN     (6 * 60 + 30)
#define DEFAULT_QUIET_VOLUME_PCT 45

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_requests;
static volatile bool s_stop;
static uint8_t *s_pcm_buf;      /* PCM_CHUNK_BYTES, or ADPCM_PCM_BYTES if larger */
static uint8_t *s_blk_buf;      /* one ADPCM block */

static rr_audio_policy_t s_policy = {
    .volume_pct = DEFAULT_VOLUME_PCT,
    .quiet_from_min = DEFAULT_QUIET_FROM_MIN,
    .quiet_to_min = DEFAULT_QUIET_TO_MIN,
    .quiet_volume_pct = DEFAULT_QUIET_VOLUME_PCT,
};

typedef struct { char path[96]; } rr_audio_req_t;

// ── Volume policy ───────────────────────────────────────────────────────────

bool rr_audio_in_quiet_hours(void)
{
    if (s_policy.quiet_from_min == RR_AUDIO_QUIET_DISABLED) return false;

    rr_rtc_time_t t;
    if (rr_rtc_get_local(&t) != ESP_OK) return false;
    // An unset clock must NOT silence anything. "Loud at the wrong moment" is
    // recoverable; "the alarm never rang because the RTC was never set" is the
    // failure this whole subsystem exists to prevent.
    rr_rtc_time_t utc;
    if (rr_rtc_get(&utc) != ESP_OK || !utc.osc_ok) return false;

    const int now = t.hour * 60 + t.minute;
    const int from = s_policy.quiet_from_min;
    const int to = s_policy.quiet_to_min;

    // The window normally wraps midnight (20:30 -> 06:30), so the two cases are
    // genuinely different comparisons, not one with a sign flip.
    if (from <= to) return now >= from && now < to;
    return now >= from || now < to;
}

uint8_t rr_audio_effective_volume(void)
{
    if (!rr_audio_in_quiet_hours()) return s_policy.volume_pct;
    return s_policy.quiet_volume_pct < s_policy.volume_pct ? s_policy.quiet_volume_pct
                                                           : s_policy.volume_pct;
}

static void policy_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) return;
    rr_audio_policy_t p;
    size_t len = sizeof(p);
    if (nvs_get_blob(h, NVS_KEY_POLICY, &p, &len) == ESP_OK && len == sizeof(p)) {
        s_policy = p;
        ESP_LOGI(TAG, "policy restored: volume %u%%, quiet %02d:%02d-%02d:%02d at %u%%",
                 p.volume_pct,
                 p.quiet_from_min < 0 ? 0 : p.quiet_from_min / 60,
                 p.quiet_from_min < 0 ? 0 : p.quiet_from_min % 60,
                 p.quiet_to_min < 0 ? 0 : p.quiet_to_min / 60,
                 p.quiet_to_min < 0 ? 0 : p.quiet_to_min % 60,
                 p.quiet_volume_pct);
    }
    nvs_close(h);
}

esp_err_t rr_audio_set_policy(const rr_audio_policy_t *p)
{
    if (p == NULL) return ESP_ERR_INVALID_ARG;
    if (p->volume_pct > 100 || p->quiet_volume_pct > 100) return ESP_ERR_INVALID_ARG;
    if (p->quiet_from_min != RR_AUDIO_QUIET_DISABLED &&
        (p->quiet_from_min < 0 || p->quiet_from_min > 1439 ||
         p->quiet_to_min < 0 || p->quiet_to_min > 1439)) {
        return ESP_ERR_INVALID_ARG;
    }

    s_policy = *p;

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "policy applied but NOT persisted — it is lost on reboot");
        return err;
    }
    err = nvs_set_blob(h, NVS_KEY_POLICY, &s_policy, sizeof(s_policy));
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);

    ESP_LOGI(TAG, "policy set: volume %u%%, quiet %s, cap %u%% (effective now %u%%)",
             s_policy.volume_pct,
             s_policy.quiet_from_min == RR_AUDIO_QUIET_DISABLED ? "off" : "on",
             s_policy.quiet_volume_pct, rr_audio_effective_volume());
    return err;
}

void rr_audio_get_policy(rr_audio_policy_t *out)
{
    if (out != NULL) *out = s_policy;
}

// ── WAV parsing ─────────────────────────────────────────────────────────────
//
// Walks the RIFF chunk list instead of assuming the PCM starts at byte 44. The
// generated clips no longer agree on a header size anyway — ADPCM's is 60 bytes
// with a `fact` chunk — and a fixed offset would play the header as if it were
// audio: a burst of noise, at alarm volume, on a child's wrist.

#define WAV_FMT_PCM   0x0001
#define WAV_FMT_IMA   0x0011

typedef struct {
    FILE *f;
    uint16_t format;
    uint32_t rate;
    uint16_t bits;
    uint16_t channels;
    uint16_t block_bytes;        /**< ADPCM only */
    uint16_t samples_per_block;  /**< ADPCM only */
    uint32_t data_bytes;
    uint32_t total_samples;      /**< from `fact`; 0 if absent */
} wav_t;

static esp_err_t wav_open(const char *path, wav_t *w)
{
    memset(w, 0, sizeof(*w));

    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "no such clip: %s — is the littlefs asset image flashed?", path);
        return ESP_ERR_NOT_FOUND;
    }

    char riff[12];
    if (fread(riff, 1, sizeof(riff), f) != sizeof(riff) ||
        memcmp(riff, "RIFF", 4) != 0 || memcmp(riff + 8, "WAVE", 4) != 0) {
        ESP_LOGE(TAG, "%s is not a RIFF/WAVE file", path);
        fclose(f);
        return ESP_ERR_INVALID_ARG;
    }

    bool have_fmt = false;
    for (;;) {
        char id[4];
        uint32_t size;
        if (fread(id, 1, 4, f) != 4 || fread(&size, 1, 4, f) != 4) break;

        if (memcmp(id, "fmt ", 4) == 0) {
            uint8_t fmt[20];
            const uint32_t want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) break;
            w->format      = (uint16_t) (fmt[0] | (fmt[1] << 8));
            w->channels    = (uint16_t) (fmt[2] | (fmt[3] << 8));
            w->rate        = (uint32_t) (fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24));
            w->block_bytes = (uint16_t) (fmt[12] | (fmt[13] << 8));
            w->bits        = (uint16_t) (fmt[14] | (fmt[15] << 8));
            if (want >= 20) w->samples_per_block = (uint16_t) (fmt[18] | (fmt[19] << 8));
            have_fmt = true;
            if (size > want) fseek(f, (long) (size - want), SEEK_CUR);
        } else if (memcmp(id, "fact", 4) == 0) {
            uint32_t n = 0;
            if (size >= 4 && fread(&n, 1, 4, f) == 4) w->total_samples = n;
            if (size > 4) fseek(f, (long) (size - 4), SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) break;
            w->f = f;
            w->data_bytes = size;

            if (w->format != WAV_FMT_PCM && w->format != WAV_FMT_IMA) {
                ESP_LOGE(TAG, "%s: format 0x%04X is neither PCM nor IMA ADPCM",
                         path, w->format);
                fclose(f);
                return ESP_ERR_NOT_SUPPORTED;
            }
            if (w->format == WAV_FMT_IMA &&
                (w->block_bytes == 0 || w->block_bytes > RR_AUDIO_ADPCM_BLOCK_BYTES ||
                 w->samples_per_block == 0 ||
                 w->samples_per_block > RR_AUDIO_ADPCM_SAMPLES_PER_BLOCK)) {
                // The decode buffers are sized from the manifest's geometry at
                // compile time, so a clip claiming a bigger block would overrun
                // them. Refuse rather than trust the file.
                ESP_LOGE(TAG, "%s: ADPCM block %u/%u samples exceeds the built-in "
                              "buffers (%d/%d)", path, w->block_bytes,
                         w->samples_per_block, RR_AUDIO_ADPCM_BLOCK_BYTES,
                         RR_AUDIO_ADPCM_SAMPLES_PER_BLOCK);
                fclose(f);
                return ESP_ERR_INVALID_SIZE;
            }
            return ESP_OK;
        } else {
            fseek(f, (long) size, SEEK_CUR);
        }
        if (size & 1) fseek(f, 1, SEEK_CUR);   /* RIFF chunks are word-aligned */
    }

    ESP_LOGE(TAG, "%s has no usable fmt/data chunk pair", path);
    fclose(f);
    return ESP_ERR_INVALID_ARG;
}

// ── IMA ADPCM decode ────────────────────────────────────────────────────────
//
// Mirrors imaDecode() in tools/watch-audio/gen-watch-audio.mjs, which is the
// reference the generator verifies its own encoder against (--verify measures
// the round trip: mean 22.9 dB, worst 15.9 dB across the voice set — normal for
// 4-bit IMA on speech, and the check fails the build below 15 dB).
//
// Every block restarts the predictor from a sample stored in its own header, so
// an error cannot propagate past one 46 ms block.

static const int16_t IMA_STEP[89] = {
    7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
    50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230,
    253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963,
    1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327,
    3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442,
    11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794,
    32767,
};
static const int8_t IMA_INDEX[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };

static inline int16_t clamp_i16(int32_t v)
{
    if (v < -32768) return -32768;
    if (v > 32767) return 32767;
    return (int16_t) v;
}

/** Decode one block into s16le. Returns the sample count written. */
static size_t ima_decode_block(const uint8_t *blk, size_t blk_len,
                               uint16_t samples_per_block, int16_t *out)
{
    if (blk_len < 4) return 0;

    int32_t pred = (int16_t) (blk[0] | (blk[1] << 8));
    int index = blk[2];
    if (index > 88) index = 88;

    size_t n = 0;
    out[n++] = (int16_t) pred;          /* the header sample IS sample 0 */

    for (uint16_t i = 0; i + 1 < samples_per_block; i++) {
        const size_t byte_off = 4 + (i >> 1);
        if (byte_off >= blk_len) break;
        const uint8_t byte = blk[byte_off];
        const uint8_t code = (i & 1) ? (uint8_t) (byte >> 4) : (uint8_t) (byte & 0x0F);

        const int32_t step = IMA_STEP[index];
        int32_t delta = step >> 3;
        if (code & 1) delta += step >> 2;
        if (code & 2) delta += step >> 1;
        if (code & 4) delta += step;

        pred = (code & 8) ? pred - delta : pred + delta;
        pred = clamp_i16(pred);
        index += IMA_INDEX[code];
        if (index < 0) index = 0;
        if (index > 88) index = 88;

        out[n++] = (int16_t) pred;
    }
    return n;
}

// ── Playback ────────────────────────────────────────────────────────────────

static void play_blocking(const char *path)
{
    wav_t w;
    if (wav_open(path, &w) != ESP_OK) return;

    const uint8_t vol = rr_audio_effective_volume();
    if (vol == 0) {
        ESP_LOGW(TAG, "volume 0%s — %s not played",
                 rr_audio_in_quiet_hours() ? " (quiet hours)" : "", path);
        fclose(w.f);
        return;
    }

    // Opened per clip and closed after. esp_codec_dev drives the power-amp
    // enable (GPIO 6) off open/close, so holding it open would leave the
    // amplifier powered between sounds — a constant draw on a watch that makes
    // a noise a handful of times a day.
    //
    // ADPCM decodes to 16-bit before it reaches I2S, so the codec is always
    // opened at 16 bits regardless of the file's 4.
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = 16,
        .channel = (uint8_t) w.channels,
        .sample_rate = w.rate,
    };

    esp_err_t err = esp_codec_dev_open(s_spk, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        fclose(w.f);
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, (int) vol);

    ESP_LOGI(TAG, "playing %s (%s, %" PRIu32 " B, %" PRIu32 " Hz, %u ch, vol %u%%%s)",
             path, w.format == WAV_FMT_IMA ? "ADPCM" : "PCM",
             w.data_bytes, w.rate, w.channels, vol,
             rr_audio_in_quiet_hours() ? ", quiet hours" : "");

    uint32_t remaining = w.data_bytes;
    uint32_t samples_left = w.total_samples;   /* 0 = unknown, PCM case */

    while (remaining > 0 && !s_stop) {
        if (w.format == WAV_FMT_IMA) {
            const size_t want = remaining < w.block_bytes ? remaining : w.block_bytes;
            const size_t got = fread(s_blk_buf, 1, want, w.f);
            if (got == 0) break;
            remaining -= got;

            size_t n = ima_decode_block(s_blk_buf, got, w.samples_per_block,
                                        (int16_t *) s_pcm_buf);
            // The final block is zero-padded to a full block by the encoder;
            // `fact` says how many samples are real, so the padding is not
            // played as a tail of silence-shaped noise.
            if (w.total_samples != 0) {
                if (n > samples_left) n = samples_left;
                samples_left -= n;
            }
            if (n == 0) break;
            if (esp_codec_dev_write(s_spk, s_pcm_buf, (int) (n * 2)) != ESP_OK) {
                ESP_LOGE(TAG, "codec write failed — stopping playback");
                break;
            }
            if (w.total_samples != 0 && samples_left == 0) break;
        } else {
            const size_t want = remaining < PCM_CHUNK_BYTES ? remaining : PCM_CHUNK_BYTES;
            const size_t got = fread(s_pcm_buf, 1, want, w.f);
            if (got == 0) break;
            remaining -= got;
            if (esp_codec_dev_write(s_spk, s_pcm_buf, (int) got) != ESP_OK) {
                ESP_LOGE(TAG, "codec write failed — stopping playback");
                break;
            }
        }
    }

    fclose(w.f);
    esp_codec_dev_close(s_spk);

    if (s_stop) ESP_LOGI(TAG, "playback stopped early");
}

esp_err_t rr_audio_selftest_decode(const char *path)
{
    // Decode a clip WITHOUT playing it, and report statistics that can be
    // compared against a host-side reference decode of the same file.
    //
    // This exists because I cannot hear the watch. A 4-bit ADPCM decoder with a
    // wrong nibble order or a mis-stepped index table produces a file-shaped
    // stream of noise that looks entirely healthy in a log — and the person who
    // finds out is a child. An order-sensitive checksum over every decoded
    // sample either matches the reference exactly or it does not.
    wav_t w;
    if (wav_open(path, &w) != ESP_OK) return ESP_FAIL;
    if (w.format != WAV_FMT_IMA) {
        ESP_LOGW(TAG, "selftest: %s is not ADPCM — nothing to check", path);
        fclose(w.f);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t remaining = w.data_bytes, samples_left = w.total_samples, n_total = 0;
    uint32_t acc = 0;
    int32_t peak = 0;
    double sq = 0;

    while (remaining > 0) {
        const size_t want = remaining < w.block_bytes ? remaining : w.block_bytes;
        const size_t got = fread(s_blk_buf, 1, want, w.f);
        if (got == 0) break;
        remaining -= got;

        size_t n = ima_decode_block(s_blk_buf, got, w.samples_per_block,
                                    (int16_t *) s_pcm_buf);
        if (w.total_samples != 0) {
            if (n > samples_left) n = samples_left;
            samples_left -= n;
        }
        const int16_t *pcm = (const int16_t *) s_pcm_buf;
        for (size_t i = 0; i < n; i++) {
            const int16_t v = pcm[i];
            acc = acc * 31u + (uint32_t) (uint16_t) v;
            const int32_t a = v < 0 ? -v : v;
            if (a > peak) peak = a;
            sq += (double) v * v;
        }
        n_total += n;
        if (w.total_samples != 0 && samples_left == 0) break;
    }
    fclose(w.f);

    const uint32_t rms = n_total ? (uint32_t) sqrt(sq / n_total) : 0;
    ESP_LOGI(TAG, "ADPCM selftest %s: samples=%" PRIu32 " peak=%" PRId32
                  " rms=%" PRIu32 " fnv31=0x%08" PRIx32,
             path, n_total, peak, rms, acc);
    return ESP_OK;
}

static void player_task(void *arg)
{
    (void) arg;
    rr_audio_req_t req;

    for (;;) {
        if (xQueueReceive(s_requests, &req, portMAX_DELAY) == pdTRUE) {
            s_stop = false;
            play_blocking(req.path);
        }
    }
}

// ── API ─────────────────────────────────────────────────────────────────────

// ═════════════════════════════════════════════════════════════════════════════
// I2S bring-up is OURS, not the BSP's — and this is a power decision (Phase 10)
//
// ⚠️ THIS IS WHAT WAS BLOCKING LIGHT SLEEP FOR THE ENTIRE DEVICE. Not the
// display, not BLE — the speaker.
//
// bsp_audio_init() calls i2s_channel_enable() on BOTH channels and never
// disables them. i2s_channel_enable() acquires an ESP_PM_APB_FREQ_MAX lock, so
// from boot onwards the chip held two of them, forever, and automatic light
// sleep could never engage for a single millisecond. It was invisible: audio
// worked perfectly, the display slept, BLE behaved, and the only symptom was a
// battery that drained as if none of the power work existed. The evidence is
// esp_pm_dump_locks() (rr_pm_dump_locks()) — two `i2s_driver APB_FREQ_MAX`
// entries with Active=1 while the watch was "asleep".
//
// The channels cannot be fixed from outside: the BSP keeps both handles in
// file-statics and exposes no accessor and no deinit — the same trap
// docs/POWER.md records for the panel handle. So this takes ownership, which is
// a faithful transcription of bsp_audio_init() + bsp_audio_codec_speaker_init()
// with two deliberate differences:
//
//   1. NO RX CHANNEL IS CREATED AT ALL. v1 never uses the microphone (§10B: the
//      ES7210 path is reserved for a possible future voice feature), so the BSP
//      was holding a PM lock and DMA buffers for a peripheral nothing reads.
//   2. THE CHANNEL IS NOT ENABLED HERE. It does not need to be: esp_codec_dev's
//      I2S data interface already calls i2s_channel_enable() on open and
//      i2s_channel_disable() on close (audio_codec_data_i2s.c), and the player
//      opens and closes per clip. So the lock is now held for the ~2 seconds a
//      sound is playing instead of for the life of the device, and the play
//      path did not have to change at all.
//
// The alarm is the only thing that makes a scheduled routine noticeable (there
// is no vibration motor, §2), so the failure mode of getting this wrong is a
// silent watch. Every error path below therefore says so in as many words.
// ═════════════════════════════════════════════════════════════════════════════
static esp_err_t audio_hw_init(void)
{
    // ⚠️ TRANSCRIBED FROM THE BSP, BECAUSE THE MACRO IS PRIVATE.
    // BSP_I2S_DUPLEX_MONO_CFG lives in esp32_c6_touch_amoled_2_06.c, not in any
    // header, so it cannot be reused — only copied. THE PIN SYMBOLS BELOW ARE
    // PUBLIC and are referenced rather than hard-coded, so a board revision that
    // moves a pin still lands here correctly; what is genuinely duplicated is
    // only the SHAPE (Philips slots, 16-bit, mono, 22.05 kHz), which is fixed by
    // the ES8311 wiring and the clip format rather than by the board.
    //
    // If audio ever comes up distorted or silent after a BSP version bump, diff
    // this against that macro first.
    const i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(22050),
        .slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT,
                                                       I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = BSP_I2S_MCLK,
            .bclk = BSP_I2S_SCLK,
            .ws   = BSP_I2S_LCLK,
            .dout = BSP_I2S_DOUT,
            .din  = BSP_I2S_DSIN,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(CONFIG_BSP_I2S_NUM,
                                                            I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;   // clear stale DMA data, as the BSP does

    // TX handle only; passing NULL for RX is what stops the mic channel from
    // being created.
    i2s_chan_handle_t tx = NULL;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s — THE ALARM WILL BE SILENT",
                 esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_init_std_mode(tx, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s — THE ALARM WILL BE SILENT",
                 esp_err_to_name(err));
        i2s_del_channel(tx);
        return err;
    }
    // NOTE THE ABSENCE of i2s_channel_enable() here. See the block comment.

    const audio_codec_i2s_cfg_t i2s_cfg = {
        .port = CONFIG_BSP_I2S_NUM,
        .tx_handle = tx,
        .rx_handle = NULL,
    };
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_cfg);
    if (data_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2s_data failed — THE ALARM WILL BE SILENT");
        i2s_del_channel(tx);
        return ESP_FAIL;
    }

    // ── ES8311 over the shared board I2C bus ────────────────────────────────
    // Phase 0 verified this chain end to end: codec at 0x18 in slave mode, power
    // amplifier on GPIO 6 driven by esp_codec_dev off open/close.
    const audio_codec_i2c_cfg_t i2c_cfg = {
        .port = BSP_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = bsp_i2c_get_handle(),
    };
    const audio_codec_ctrl_if_t *ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    if (ctrl_if == NULL) {
        ESP_LOGE(TAG, "audio_codec_new_i2c_ctrl failed — THE ALARM WILL BE SILENT");
        return ESP_FAIL;
    }

    const es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = ctrl_if,
        .gpio_if = audio_codec_new_gpio(),
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_DAC,
        .pa_pin = BSP_POWER_AMP_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = { .pa_voltage = 5.0, .codec_dac_voltage = 3.3 },
    };
    const audio_codec_if_t *es8311 = es8311_codec_new(&es8311_cfg);
    if (es8311 == NULL) {
        ESP_LOGE(TAG, "es8311_codec_new failed — THE ALARM WILL BE SILENT");
        return ESP_FAIL;
    }

    const esp_codec_dev_cfg_t dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_OUT,
        .codec_if = es8311,
        .data_if = data_if,
    };
    s_spk = esp_codec_dev_new(&dev_cfg);
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "esp_codec_dev_new failed — THE ALARM WILL BE SILENT");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "I2S TX-only, channel left DISABLED until a clip plays "
                  "(no APB lock held while idle; no RX channel created)");
    return ESP_OK;
}

esp_err_t rr_audio_init(void)
{
    esp_err_t err = audio_hw_init();
    if (err != ESP_OK) return err;

    const size_t pcm_cap = ADPCM_PCM_BYTES > PCM_CHUNK_BYTES ? ADPCM_PCM_BYTES
                                                             : PCM_CHUNK_BYTES;
    s_pcm_buf = malloc(pcm_cap);
    s_blk_buf = malloc(RR_AUDIO_ADPCM_BLOCK_BYTES);
    if (s_pcm_buf == NULL || s_blk_buf == NULL) {
        ESP_LOGE(TAG, "could not allocate the %u B playback buffers", (unsigned) pcm_cap);
        return ESP_ERR_NO_MEM;
    }

    s_requests = xQueueCreate(REQUEST_DEPTH, sizeof(rr_audio_req_t));
    if (s_requests == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(player_task, "rr_audio", PLAYER_STACK, NULL, PLAYER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the player task");
        return ESP_ERR_NO_MEM;
    }

    policy_load();
    ESP_LOGI(TAG, "ES8311 ready — volume %u%% (effective %u%%), quiet hours %s, "
                  "buffers %u+%u B",
             s_policy.volume_pct, rr_audio_effective_volume(),
             s_policy.quiet_from_min == RR_AUDIO_QUIET_DISABLED ? "off" : "on",
             (unsigned) pcm_cap, RR_AUDIO_ADPCM_BLOCK_BYTES);
    return ESP_OK;
}

bool rr_audio_is_ready(void) { return s_spk != NULL && s_requests != NULL; }

esp_err_t rr_audio_play_file(const char *path)
{
    if (path == NULL || path[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (!rr_audio_is_ready()) {
        ESP_LOGE(TAG, "audio not initialised — %s NOT played", path);
        return ESP_ERR_INVALID_STATE;
    }

    rr_audio_req_t req;
    strlcpy(req.path, path, sizeof(req.path));

    // Drop rather than block: a step blip that cannot get a queue slot is worth
    // less than never stalling the caller (which may be the LVGL task).
    if (xQueueSend(s_requests, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "audio queue full — dropped %s", path);
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t rr_audio_play_tone(rr_tone_id_t id)
{
    if ((int) id < 0 || (int) id >= RR_AUDIO_TONE_COUNT) return ESP_ERR_INVALID_ARG;
    return rr_audio_play_file(RR_TONE_TABLE[id]);
}

esp_err_t rr_audio_play_voice(const char *emoji, const char *label, const char *lang)
{
    const char *p = rr_audio_voice_path(emoji, label, lang);
    if (p == NULL) return ESP_ERR_NOT_FOUND;
    return rr_audio_play_file(p);
}

esp_err_t rr_audio_play_alarm(void)
{
    if (!rr_audio_is_ready()) {
        ESP_LOGE(TAG, "audio not initialised — THE ALARM IS SILENT");
        return ESP_ERR_INVALID_STATE;
    }
    // Preempt: stop what is playing and clear the queue, so the alarm is next
    // rather than third in line behind step effects.
    s_stop = true;
    xQueueReset(s_requests);
    return rr_audio_play_file(RR_TONE_TABLE[RR_TONE_ALARM]);
}

void rr_audio_stop(void)
{
    s_stop = true;
    if (s_requests != NULL) xQueueReset(s_requests);
}
