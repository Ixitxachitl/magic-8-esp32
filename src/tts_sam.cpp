/*
 *  tts_sam.cpp – Text-to-speech via SAM → I2S (ES8311 DAC)
 *
 *  SAM generates 22050 Hz unsigned 8-bit audio.  We collect it into a
 *  PSRAM buffer, resample to 16000 Hz stereo 16-bit, and write it out
 *  through the shared I2S bus.
 */
#include "tts_sam.h"
#include "mic_recorder.h"
#include "pin_config.h"

#include <Arduino.h>
#include <ESP8266SAM.h>
#include <AudioOutput.h>
#include "ESP_I2S.h"
#include <esp_heap_caps.h>
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

/* ── PSRAM buffer AudioOutput ──────────────────────────────────── */
class SamPSRAMBuffer : public AudioOutput {
public:
    int16_t *buf;
    size_t   len;      /* int16_t samples written (stereo: L,R,L,R…) */
    size_t   cap;      /* max int16_t count */

    SamPSRAMBuffer() : buf(nullptr), len(0), cap(0) {}
    ~SamPSRAMBuffer() override { free_buf(); }

    void free_buf() { if (buf) { heap_caps_free(buf); buf = nullptr; } len = 0; }

    bool begin() override {
        free_buf();
        /* SAM speaks ~4 words/sec; budget 6 seconds at 22050 Hz stereo 16-bit */
        cap = 22050 * 6 * 2;          /* stereo sample count */
        buf = (int16_t *)heap_caps_malloc(cap * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!buf) { Serial.println("[TTS] PSRAM alloc failed"); return false; }
        len = 0;
        return true;
    }

    bool ConsumeSample(int16_t sample[2]) override {
        if (!buf || len + 2 > cap) return false;
        buf[len++] = sample[0];
        buf[len++] = sample[1];
        return true;
    }

    bool stop() override { return true; }
};

static SamPSRAMBuffer     s_aoBuf;
static I2SClass          *s_i2s = nullptr;

/* ── Resample 22050→16000 Hz (linear interpolation, stereo) ───── */
static int16_t *resample_22050_to_16000(const int16_t *src, size_t src_samples,
                                         size_t *out_samples)
{
    /* src_samples is total int16_t count (L+R pairs), so frames = src_samples/2 */
    size_t src_frames = src_samples / 2;
    size_t dst_frames = (size_t)((double)src_frames * 16000.0 / 22050.0);
    size_t dst_samples = dst_frames * 2;

    int16_t *dst = (int16_t *)heap_caps_malloc(dst_samples * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    if (!dst) { *out_samples = 0; return nullptr; }

    double ratio = (double)src_frames / (double)dst_frames;
    for (size_t i = 0; i < dst_frames; i++) {
        double pos  = i * ratio;
        size_t idx  = (size_t)pos;
        double frac = pos - idx;
        if (idx + 1 >= src_frames) idx = src_frames - 2;

        /* Left channel */
        int32_t l0 = src[idx * 2];
        int32_t l1 = src[(idx + 1) * 2];
        dst[i * 2] = (int16_t)(l0 + (int32_t)(frac * (l1 - l0)));

        /* Right channel */
        int32_t r0 = src[idx * 2 + 1];
        int32_t r1 = src[(idx + 1) * 2 + 1];
        dst[i * 2 + 1] = (int16_t)(r0 + (int32_t)(frac * (r1 - r0)));
    }

    *out_samples = dst_samples;
    return dst;
}

/* ── Public API ────────────────────────────────────────────────── */

void tts_init(void)
{
    s_i2s = mic_recorder_get_i2s();
    Serial.printf("[TTS] SAM init (i2s=%p txChan=%p)\n",
                  s_i2s, s_i2s ? (void *)s_i2s->txChan() : nullptr);
}

/* ── SAM task (runs with 32 KB stack to avoid core panic) ──────── */
struct sam_task_ctx {
    char              text[204];
    SamPSRAMBuffer *aoBuf;
    SemaphoreHandle_t  done;
    bool               ok;
};

static void sam_task(void *param)
{
    sam_task_ctx *ctx = (sam_task_ctx *)param;
    ESP8266SAM sam;
    sam.SetVoice(ESP8266SAM::VOICE_SAM);
    ctx->ok = sam.Say(ctx->aoBuf, ctx->text);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

void tts_say(const char *text)
{
    if (!s_i2s || !s_i2s->txChan() || !text || !text[0]) return;

    /* SAM's TextToPhonemes crashes on many non-alpha characters.
       Only keep letters, digits, spaces, and a few safe punctuation marks.
       Truncate to 200 chars (SAM limit is 254 but leave headroom). */
    char clean[204];
    int j = 0;
    bool last_space = false;
    for (int i = 0; text[i] && j < 200; i++) {
        char c = text[i];
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                    (c >= '0' && c <= '9') || c == ' ' || c == '.' ||
                    c == ',' || c == '!' || c == '?' || c == '\'' || c == '-';
        if (!keep) c = ' ';
        /* collapse multiple spaces */
        if (c == ' ') {
            if (last_space || j == 0) continue;
            last_space = true;
        } else {
            last_space = false;
        }
        clean[j++] = c;
    }
    /* trim trailing space */
    while (j > 0 && clean[j - 1] == ' ') j--;
    clean[j] = '\0';
    if (j == 0) return;

    Serial.printf("[TTS] Say: \"%s\"\n", clean);

    /* Run SAM in a separate task with 32 KB stack */
    sam_task_ctx ctx;
    strncpy(ctx.text, clean, sizeof(ctx.text));
    ctx.aoBuf = &s_aoBuf;
    ctx.done  = xSemaphoreCreateBinary();
    ctx.ok    = false;

    if (!ctx.done) {
        Serial.println("[TTS] semaphore create failed");
        return;
    }

    BaseType_t rc = xTaskCreatePinnedToCore(
        sam_task, "sam_tts", 32768, &ctx, 1, NULL, 1);
    if (rc != pdPASS) {
        Serial.println("[TTS] task create failed");
        vSemaphoreDelete(ctx.done);
        return;
    }

    /* Wait up to 10 seconds for SAM to finish */
    if (xSemaphoreTake(ctx.done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        Serial.println("[TTS] SAM timed out");
        vSemaphoreDelete(ctx.done);
        s_aoBuf.free_buf();
        return;
    }
    vSemaphoreDelete(ctx.done);

    if (!ctx.ok) {
        Serial.println("[TTS] SAM Say() failed");
        s_aoBuf.free_buf();
        return;
    }

    Serial.printf("[TTS] SAM produced %u stereo samples (%u ms at 22050)\n",
                  (unsigned)(s_aoBuf.len / 2),
                  (unsigned)(s_aoBuf.len / 2 * 1000 / 22050));

    /* Resample 22050→16000 Hz */
    size_t out_samples = 0;
    int16_t *resampled = resample_22050_to_16000(s_aoBuf.buf, s_aoBuf.len, &out_samples);
    s_aoBuf.free_buf();

    if (!resampled || out_samples == 0) {
        Serial.println("[TTS] Resample failed");
        return;
    }

    /* Write to I2S */
    size_t bytes = out_samples * sizeof(int16_t);
    Serial.printf("[TTS] Writing %u bytes (%.1f ms at 16kHz)\n",
                  (unsigned)bytes,
                  (float)(out_samples / 2) * 1000.0f / 16000.0f);

    size_t written = s_i2s->write((uint8_t *)resampled, bytes);
    Serial.printf("[TTS] Wrote %u / %u bytes\n", (unsigned)written, (unsigned)bytes);

    heap_caps_free(resampled);
}
