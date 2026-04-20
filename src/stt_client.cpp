/*
 *  stt_client.cpp – Groq Whisper speech-to-text
 *
 *  Converts 16 kHz mono PCM → WAV, uploads as multipart/form-data
 *  to /audio/transcriptions on a background FreeRTOS task.
 */
#include "stt_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <string.h>

/* ── config ────────────────────────────────────────────────────── */
static String cfg_url;
static String cfg_key;
static String cfg_model;

/* ── inter-task comms ──────────────────────────────────────────── */
static volatile bool     stt_pending = false;
static volatile bool     stt_ready   = false;
static String            stt_text;
static SemaphoreHandle_t stt_mutex   = NULL;

/* task parameters (copied into PSRAM before task launch) */
struct SttTaskParams {
    int16_t *samples;      /* copied into this struct's trailing data */
    size_t   sample_count;
};

static const char *BOUNDARY = "ESP32AudioBndry";

/* ── WAV header helper (44 bytes, 16 kHz mono 16-bit PCM) ──────── */
static void write_wav_header(uint8_t *dst, size_t data_bytes)
{
    uint32_t file_sz  = data_bytes + 36;
    uint32_t sr       = 16000;
    uint32_t byte_rt  = 32000;     /* sr * 1 * 2 */
    uint16_t blk_alg  = 2;
    uint16_t bps      = 16;
    uint16_t channels = 1;
    uint16_t pcm      = 1;
    uint32_t fmt_sz   = 16;

    memcpy(dst +  0, "RIFF", 4);
    memcpy(dst +  4, &file_sz, 4);
    memcpy(dst +  8, "WAVE", 4);
    memcpy(dst + 12, "fmt ", 4);
    memcpy(dst + 16, &fmt_sz,   4);
    memcpy(dst + 20, &pcm,      2);
    memcpy(dst + 22, &channels, 2);
    memcpy(dst + 24, &sr,       4);
    memcpy(dst + 28, &byte_rt,  4);
    memcpy(dst + 32, &blk_alg,  2);
    memcpy(dst + 34, &bps,      2);
    memcpy(dst + 36, "data", 4);
    memcpy(dst + 40, &data_bytes, 4);
}

/* ── build multipart/form-data body into `dst` ─────────────────── */
static size_t build_body(uint8_t *dst, const int16_t *samples, size_t count)
{
    size_t pos = 0;
    size_t pcm_bytes = count * sizeof(int16_t);

#define APPEND_STR(s) do { size_t l = strlen(s); memcpy(dst + pos, s, l); pos += l; } while(0)

    /* Part 1 – audio file */
    APPEND_STR("--"); APPEND_STR(BOUNDARY); APPEND_STR("\r\n");
    APPEND_STR("Content-Disposition: form-data; name=\"file\"; filename=\"audio.wav\"\r\n");
    APPEND_STR("Content-Type: audio/wav\r\n\r\n");

    write_wav_header(dst + pos, pcm_bytes);
    pos += 44;
    memcpy(dst + pos, samples, pcm_bytes);
    pos += pcm_bytes;
    APPEND_STR("\r\n");

    /* Part 2 – model */
    APPEND_STR("--"); APPEND_STR(BOUNDARY); APPEND_STR("\r\n");
    APPEND_STR("Content-Disposition: form-data; name=\"model\"\r\n\r\n");
    {
        const char *m = cfg_model.c_str();
        size_t ml = strlen(m);
        memcpy(dst + pos, m, ml); pos += ml;
    }
    APPEND_STR("\r\n");

    /* Part 3 – response_format */
    APPEND_STR("--"); APPEND_STR(BOUNDARY); APPEND_STR("\r\n");
    APPEND_STR("Content-Disposition: form-data; name=\"response_format\"\r\n\r\n");
    APPEND_STR("text\r\n");

    /* Part 4 – language */
    APPEND_STR("--"); APPEND_STR(BOUNDARY); APPEND_STR("\r\n");
    APPEND_STR("Content-Disposition: form-data; name=\"language\"\r\n\r\n");
    APPEND_STR("en\r\n");

    /* closing boundary */
    APPEND_STR("--"); APPEND_STR(BOUNDARY); APPEND_STR("--\r\n");

#undef APPEND_STR
    return pos;
}

/* ── background task ───────────────────────────────────────────── */
static void stt_task(void *param)
{
    SttTaskParams *p = (SttTaskParams *)param;
    String result;

    Serial.printf("[STT] Task started  samples=%u (%.1f s)\n",
                  (unsigned)p->sample_count,
                  (float)p->sample_count / 16000.0f);

    /* alloc body buffer in PSRAM */
    size_t pcm_bytes  = p->sample_count * sizeof(int16_t);
    size_t body_alloc = pcm_bytes + 44 + 1024;          /* WAV + multipart overhead */
    uint8_t *body = (uint8_t *)heap_caps_malloc(body_alloc, MALLOC_CAP_SPIRAM);
    if (!body) {
        Serial.println("[STT] Body alloc failed");
        result = "";
        goto done;
    }

    {
        size_t body_len = build_body(body, p->samples, p->sample_count);
        Serial.printf("[STT] Body built: %u bytes\n", (unsigned)body_len);

        WiFiClientSecure *client = new WiFiClientSecure();
        if (!client) {
            Serial.println("[STT] WiFiClientSecure alloc failed");
            result = "";
            heap_caps_free(body);
            goto done;
        }
        client->setInsecure();

        HTTPClient http;
        String url = cfg_url;
        if (!url.endsWith("/")) url += "/";
        url += "audio/transcriptions";
        Serial.printf("[STT] POST %s\n", url.c_str());

        if (http.begin(*client, url)) {
            http.addHeader("Content-Type",
                           String("multipart/form-data; boundary=") + BOUNDARY);
            if (cfg_key.length() > 0)
                http.addHeader("Authorization", "Bearer " + cfg_key);
            http.setTimeout(30000);

            unsigned long t0 = millis();
            int code = http.sendRequest("POST", body, body_len);
            unsigned long elapsed = millis() - t0;
            Serial.printf("[STT] HTTP %d  (%lu ms)\n", code, elapsed);

            if (code == 200) {
                result = http.getString();
                result.trim();
                Serial.printf("[STT] Transcript: \"%s\"\n", result.c_str());
            } else {
                String errBody = http.getString();
                Serial.printf("[STT] Error %d: %s\n", code, errBody.c_str());
                result = "";
            }
            http.end();
        } else {
            Serial.println("[STT] http.begin() failed");
            result = "";
        }
        delete client;
        heap_caps_free(body);
    }

done:
    /* free the param block (samples copy) */
    heap_caps_free(p);

    xSemaphoreTake(stt_mutex, portMAX_DELAY);
    stt_text    = result;
    stt_ready   = true;
    stt_pending = false;
    xSemaphoreGive(stt_mutex);

    vTaskDelete(NULL);
}

/* ── public API ────────────────────────────────────────────────── */

void stt_init(const String &api_url, const String &api_key)
{
    cfg_url = api_url;
    cfg_key = api_key;
    /* Pick the right Whisper model based on provider URL */
    if (api_url.indexOf("openai.com") >= 0) {
        cfg_model = "whisper-1";
    } else {
        cfg_model = "whisper-large-v3-turbo";
    }
    if (!stt_mutex) stt_mutex = xSemaphoreCreateMutex();
    Serial.println("[STT] Initialized");
    Serial.printf("[STT]   URL: %s  model: %s\n", api_url.c_str(), cfg_model.c_str());
}

void stt_start_transcribe(const int16_t *samples, size_t sample_count)
{
    if (stt_pending) {
        Serial.println("[STT] Already busy – skipping");
        return;
    }
    if (sample_count == 0) {
        Serial.println("[STT] No samples – skipping");
        return;
    }

    /* copy samples so the caller's buffer can keep recording */
    size_t pcm_bytes  = sample_count * sizeof(int16_t);
    size_t alloc_size = sizeof(SttTaskParams) + pcm_bytes;
    SttTaskParams *p  = (SttTaskParams *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
    if (!p) {
        Serial.println("[STT] Param alloc failed");
        return;
    }
    p->sample_count = sample_count;
    p->samples      = (int16_t *)((uint8_t *)p + sizeof(SttTaskParams));
    memcpy(p->samples, samples, pcm_bytes);

    stt_pending = true;
    stt_ready   = false;

    xTaskCreatePinnedToCore(stt_task, "stt", 16384, p, 1, NULL, 0);
    Serial.printf("[STT] Request started (%u samples)\n", (unsigned)sample_count);
}

bool stt_is_busy(void) { return stt_pending; }

bool stt_check_result(String &out)
{
    if (!stt_ready) return false;
    xSemaphoreTake(stt_mutex, portMAX_DELAY);
    out       = stt_text;
    stt_ready = false;
    xSemaphoreGive(stt_mutex);
    return true;
}
