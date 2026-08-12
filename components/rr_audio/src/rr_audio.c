// rr_audio — minimum viable ES8311 playback so the scheduler can be heard.
// See rr_audio.h for what this deliberately does NOT do (that is Phase 8).

#include "rr_audio.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "bsp/esp-bsp.h"
#include "esp_codec_dev.h"

static const char *TAG = "rr_audio";

// Streamed in chunks rather than loaded whole: tone_alarm.wav is ~107 KiB of
// PCM and the free heap at this point is ~180 KiB. Loading a clip would be a
// third of the machine's remaining RAM to play a two-second sound.
#define CHUNK_BYTES 2048

#define PLAYER_STACK 4096
#define PLAYER_PRIO  4          /* below LVGL: a late tone beats a torn frame */

#define DEFAULT_VOLUME_PCT 70   /* see rr_audio.h on parent control / quiet hours */

static esp_codec_dev_handle_t s_spk;
static QueueHandle_t s_requests;         /* depth 1, holds a path */
static volatile bool s_stop;
static uint8_t s_volume = DEFAULT_VOLUME_PCT;

typedef struct { char path[96]; } rr_audio_req_t;

// ── WAV parsing ─────────────────────────────────────────────────────────────
//
// Walks the RIFF chunk list instead of assuming the PCM starts at byte 44.
// The generated clips do put it there today, but a fixed offset turns any
// future encoder change (a LIST/INFO chunk is the usual one) into playback of
// the header as if it were audio — a burst of noise, at alarm volume, on a
// child's wrist. The walk costs a few lines.
static esp_err_t wav_open(const char *path, FILE **out_f, uint32_t *out_bytes,
                          uint32_t *out_rate, uint8_t *out_bits, uint8_t *out_ch)
{
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
            uint8_t fmt[16];
            const uint32_t want = size < sizeof(fmt) ? size : sizeof(fmt);
            if (fread(fmt, 1, want, f) != want) break;
            const uint16_t format = (uint16_t) (fmt[0] | (fmt[1] << 8));
            if (format != 1) {   /* 1 = PCM; there is no decoder in this build */
                ESP_LOGE(TAG, "%s is compressed (format %u) — PCM only", path, format);
                fclose(f);
                return ESP_ERR_NOT_SUPPORTED;
            }
            *out_ch   = fmt[2];
            *out_rate = (uint32_t) (fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | (fmt[7] << 24));
            *out_bits = fmt[14];
            have_fmt = true;
            if (size > want) fseek(f, (long) (size - want), SEEK_CUR);
        } else if (memcmp(id, "data", 4) == 0) {
            if (!have_fmt) break;      /* data before fmt — malformed */
            *out_f = f;
            *out_bytes = size;
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

static void play_blocking(const char *path)
{
    FILE *f = NULL;
    uint32_t remaining = 0, rate = 0;
    uint8_t bits = 0, ch = 0;

    if (wav_open(path, &f, &remaining, &rate, &bits, &ch) != ESP_OK) return;

    if (s_volume == 0) {
        ESP_LOGW(TAG, "volume is 0 — %s not played", path);
        fclose(f);
        return;
    }

    // Opened per clip and closed after. esp_codec_dev drives the power-amp
    // enable (GPIO 6) off open/close, so holding the device open would leave
    // the amplifier powered between sounds — a constant draw on a watch that
    // makes a noise a handful of times a day.
    esp_codec_dev_sample_info_t fs = {
        .bits_per_sample = bits,
        .channel = ch,
        .sample_rate = rate,
    };

    esp_err_t err = esp_codec_dev_open(s_spk, &fs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "codec open failed: %s", esp_err_to_name(err));
        fclose(f);
        return;
    }
    esp_codec_dev_set_out_vol(s_spk, (int) s_volume);

    uint8_t *chunk = malloc(CHUNK_BYTES);
    if (chunk == NULL) {
        ESP_LOGE(TAG, "OOM — cannot play %s", path);
        esp_codec_dev_close(s_spk);
        fclose(f);
        return;
    }

    ESP_LOGI(TAG, "playing %s (%" PRIu32 " bytes, %" PRIu32 " Hz, %u-bit, %u ch, vol %u%%)",
             path, remaining, rate, bits, ch, s_volume);

    while (remaining > 0 && !s_stop) {
        const size_t want = remaining < CHUNK_BYTES ? remaining : CHUNK_BYTES;
        const size_t got = fread(chunk, 1, want, f);
        if (got == 0) break;
        if (esp_codec_dev_write(s_spk, chunk, (int) got) != ESP_OK) {
            ESP_LOGE(TAG, "codec write failed — stopping playback");
            break;
        }
        remaining -= got;
    }

    free(chunk);
    fclose(f);
    esp_codec_dev_close(s_spk);

    if (s_stop) ESP_LOGI(TAG, "playback stopped early");
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

esp_err_t rr_audio_init(void)
{
    esp_err_t err = bsp_audio_init(NULL);   // NULL = mono, 16-bit, 22050 Hz
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_init failed: %s — THE ALARM WILL BE SILENT",
                 esp_err_to_name(err));
        return err;
    }

    s_spk = bsp_audio_codec_speaker_init();
    if (s_spk == NULL) {
        ESP_LOGE(TAG, "ES8311 speaker init returned NULL — THE ALARM WILL BE SILENT");
        return ESP_FAIL;
    }

    s_requests = xQueueCreate(1, sizeof(rr_audio_req_t));
    if (s_requests == NULL) return ESP_ERR_NO_MEM;

    if (xTaskCreate(player_task, "rr_audio", PLAYER_STACK, NULL, PLAYER_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "could not start the player task");
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "ES8311 ready (default volume %u%%)", s_volume);
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

    // Overwrite rather than block or stack: xQueueOverwrite on a depth-1 queue
    // means the newest request wins and no caller ever waits on the speaker.
    xQueueOverwrite(s_requests, &req);
    return ESP_OK;
}

esp_err_t rr_audio_play_tone(rr_tone_id_t id)
{
    if ((int) id < 0 || (int) id >= RR_AUDIO_TONE_COUNT) return ESP_ERR_INVALID_ARG;
    return rr_audio_play_file(RR_TONE_TABLE[id]);
}

void rr_audio_stop(void) { s_stop = true; }

void rr_audio_set_volume(uint8_t pct)
{
    s_volume = pct > 100 ? 100 : pct;
    ESP_LOGI(TAG, "volume set to %u%%", s_volume);
}

uint8_t rr_audio_get_volume(void) { return s_volume; }
