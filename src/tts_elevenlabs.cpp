/*
 *  tts_elevenlabs.cpp – Text-to-speech via ElevenLabs API
 *
 *  POST https://api.elevenlabs.io/v1/text-to-speech/{voice_id}?output_format=pcm_16000
 *  Headers: xi-api-key, Content-Type: application/json
 *  Body: {"text": "...", "model_id": "eleven_turbo_v2_5"}
 *  Response: raw signed 16-bit LE mono PCM at 16 kHz
 */
#include "tts_elevenlabs.h"
#include "mic_recorder.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "ESP_I2S.h"
#include <esp_heap_caps.h>

static String cfg_key;
static String cfg_voice_id = "21m00Tcm4TlvDq8ikWAM";  /* Rachel */

static I2SClass *s_i2s = nullptr;

/* ── Public API ────────────────────────────────────────────────── */

void tts_elevenlabs_init(const String &api_key)
{
    cfg_key = api_key;
    s_i2s = mic_recorder_get_i2s();
    Serial.printf("[TTS-11L] Init (voice=%s i2s=%p)\n",
                  cfg_voice_id.c_str(), s_i2s);
}

void tts_elevenlabs_set_voice(const String &voice_id)
{
    if (voice_id.length() > 0) cfg_voice_id = voice_id;
    Serial.printf("[TTS-11L] Voice set to: %s\n", cfg_voice_id.c_str());
}

void tts_elevenlabs_say(const char *text)
{
    if (!s_i2s || !s_i2s->txChan() || !text || !text[0]) return;

    /* Clean & truncate */
    char clean[504];
    int j = 0;
    for (int i = 0; text[i] && j < 500; i++) {
        char c = text[i];
        if (c == '\n') c = ' ';
        clean[j++] = c;
    }
    clean[j] = '\0';
    if (j == 0) return;

    Serial.printf("[TTS-11L] Say: \"%s\" (voice=%s)\n", clean, cfg_voice_id.c_str());

    /* Build JSON body */
    JsonDocument doc;
    doc["text"]     = clean;
    doc["model_id"] = "eleven_turbo_v2_5";

    String body;
    serializeJson(doc, body);

    /* Build URL: /v1/text-to-speech/{voice_id}?output_format=pcm_16000 */
    String url = "https://api.elevenlabs.io/v1/text-to-speech/";
    url += cfg_voice_id;
    url += "?output_format=pcm_16000";

    Serial.printf("[TTS-11L] POST %s\n", url.c_str());

    /* HTTPS request */
    WiFiClientSecure *client = new WiFiClientSecure();
    if (!client) {
        Serial.println("[TTS-11L] WiFiClientSecure alloc failed");
        return;
    }
    client->setInsecure();

    size_t total_read = 0;
    uint8_t *pcm_buf  = NULL;
    int content_len    = 0;

    {
        HTTPClient http;

        if (!http.begin(*client, url)) {
            Serial.println("[TTS-11L] http.begin() failed");
            delete client;
            return;
        }

        http.addHeader("Content-Type", "application/json");
        http.addHeader("xi-api-key", cfg_key);
        http.setTimeout(15000);

        unsigned long t0 = millis();
        int code = http.sendRequest("POST", body);
        unsigned long elapsed = millis() - t0;
        Serial.printf("[TTS-11L] HTTP %d (%lu ms)\n", code, elapsed);

        if (code != 200) {
            String errBody = http.getString();
            Serial.printf("[TTS-11L] Error: %s\n", errBody.c_str());
            http.end();
            delete client;
            return;
        }

        /* Stream raw PCM into PSRAM */
        content_len = http.getSize();
        Serial.printf("[TTS-11L] Response size: %d bytes\n", content_len);

        size_t alloc_size = content_len > 0 ? (size_t)content_len : 256 * 1024;
        pcm_buf = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
        if (!pcm_buf) {
            Serial.println("[TTS-11L] PSRAM alloc failed");
            http.end();
            delete client;
            return;
        }

        WiFiClient *stream = http.getStreamPtr();
        uint8_t tmp[1024];
        unsigned long last_data_ms = millis();
        while (http.connected() && (content_len < 0 || (int)total_read < content_len)) {
            int avail = stream->available();
            if (avail <= 0) {
                if (content_len < 0) {
                    unsigned long idle_ms = millis() - last_data_ms;
                    if ((total_read > 1000 && idle_ms > 500) || idle_ms > 3000) {
                        Serial.println("[TTS-11L] Chunked read timeout – assuming complete");
                        break;
                    }
                }
                delay(1);
                continue;
            }
            last_data_ms = millis();
            int to_read = avail > (int)sizeof(tmp) ? (int)sizeof(tmp) : avail;
            int got = stream->readBytes(tmp, to_read);
            if (got <= 0) break;
            if (total_read + got > alloc_size) break;
            memcpy(pcm_buf + total_read, tmp, got);
            total_read += got;
        }
        http.end();
    }
    delete client;

    Serial.printf("[TTS-11L] Downloaded %u bytes raw PCM\n", (unsigned)total_read);

    if (total_read < 100) {
        Serial.println("[TTS-11L] Too little data, skipping");
        heap_caps_free(pcm_buf);
        return;
    }

    /* Response is mono 16-bit 16kHz — duplicate to stereo for I2S */
    size_t mono_frames = total_read / sizeof(int16_t);
    size_t stereo_bytes = mono_frames * 2 * sizeof(int16_t);
    int16_t *stereo = (int16_t *)heap_caps_malloc(stereo_bytes, MALLOC_CAP_SPIRAM);
    if (!stereo) {
        Serial.println("[TTS-11L] Stereo alloc failed");
        heap_caps_free(pcm_buf);
        return;
    }

    const int16_t *mono = (const int16_t *)pcm_buf;
    for (size_t i = 0; i < mono_frames; i++) {
        stereo[i * 2]     = mono[i];  /* L */
        stereo[i * 2 + 1] = mono[i];  /* R */
    }
    heap_caps_free(pcm_buf);

    Serial.printf("[TTS-11L] Playing %u bytes (%.1f ms at 16kHz)\n",
                  (unsigned)stereo_bytes, (float)mono_frames * 1000.0f / 16000.0f);

    /* Write in small chunks */
    size_t offset = 0;
    const size_t CHUNK = 4096;
    while (offset < stereo_bytes) {
        size_t n = stereo_bytes - offset;
        if (n > CHUNK) n = CHUNK;
        s_i2s->write((uint8_t *)stereo + offset, n);
        offset += n;
    }
    heap_caps_free(stereo);
}
