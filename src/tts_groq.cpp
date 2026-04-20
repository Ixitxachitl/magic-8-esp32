/*
 *  tts_groq.cpp – Text-to-speech via Groq Orpheus API
 *
 *  POST to /audio/speech with JSON body, receive WAV audio,
 *  parse header, resample if needed, play through shared I2S bus.
 */
#include "tts_groq.h"
#include "mic_recorder.h"
#include "pin_config.h"

#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <WiFiClient.h>
#include <ArduinoJson.h>
#include "ESP_I2S.h"
#include <esp_heap_caps.h>
#include <math.h>

static String cfg_url;
static String cfg_key;
static String cfg_voice = "tara";
static String cfg_model = "canopylabs/orpheus-v1-english";

static I2SClass *s_i2s = nullptr;

/* ── WAV parser ────────────────────────────────────────────────── */
struct WavInfo {
    uint16_t channels;
    uint32_t sample_rate;
    uint16_t bits_per_sample;
    const uint8_t *data;
    size_t data_len;
};

static bool parse_wav(const uint8_t *buf, size_t len, WavInfo *info)
{
    if (len < 44) return false;

    /* Debug: dump first 16 bytes to identify format */
    Serial.printf("[TTS-GROQ] WAV header: %02X %02X %02X %02X  %02X %02X %02X %02X  "
                  "%02X %02X %02X %02X  %02X %02X %02X %02X\n",
                  buf[0],buf[1],buf[2],buf[3], buf[4],buf[5],buf[6],buf[7],
                  buf[8],buf[9],buf[10],buf[11], buf[12],buf[13],buf[14],buf[15]);

    if (memcmp(buf, "RIFF", 4) != 0) {
        Serial.println("[TTS-GROQ] Not RIFF");
        return false;
    }
    if (memcmp(buf + 8, "WAVE", 4) != 0) {
        Serial.println("[TTS-GROQ] Not WAVE");
        return false;
    }

    /* Walk chunks to find "fmt " and "data" */
    size_t pos = 12;
    bool got_fmt = false, got_data = false;
    while (pos + 8 <= len) {
        uint32_t chunk_sz;
        memcpy(&chunk_sz, buf + pos + 4, 4);

        char tag[5] = {(char)buf[pos],(char)buf[pos+1],(char)buf[pos+2],(char)buf[pos+3],0};
        Serial.printf("[TTS-GROQ] Chunk '%s' size=%u at pos=%u\n", tag, chunk_sz, (unsigned)pos);

        if (memcmp(buf + pos, "fmt ", 4) == 0 && pos + 8 + 16 <= len) {
            memcpy(&info->channels, buf + pos + 10, 2);
            memcpy(&info->sample_rate, buf + pos + 12, 4);
            memcpy(&info->bits_per_sample, buf + pos + 22, 2);
            got_fmt = true;
        }
        if (memcmp(buf + pos, "data", 4) == 0) {
            info->data = buf + pos + 8;
            /* chunk_sz may be 0 or 0xFFFFFFFF for streamed WAV */
            if (chunk_sz == 0 || chunk_sz > len - (pos + 8)) {
                info->data_len = len - (pos + 8);
            } else {
                info->data_len = chunk_sz;
            }
            got_data = true;
            if (got_fmt) return true;
        }

        /* Prevent infinite loop on zero-size or huge chunks */
        size_t advance = 8 + (size_t)chunk_sz;
        if (chunk_sz & 1) advance++;  /* word-align */
        if (advance == 0 || pos + advance <= pos) break;  /* overflow guard */
        pos += advance;
    }

    /* If we found both but data came before fmt, still valid */
    if (got_fmt && got_data) return true;

    Serial.printf("[TTS-GROQ] Parse incomplete: got_fmt=%d got_data=%d\n", got_fmt, got_data);
    return false;
}

/* ── Resample to 16kHz stereo 16-bit (linear interp) ──────────── */
static int16_t *resample_to_16k_stereo(const int16_t *src, size_t src_frames,
                                        uint16_t src_channels, uint32_t src_rate,
                                        size_t *out_frames)
{
    size_t dst_frames = (size_t)((double)src_frames * 16000.0 / src_rate);
    if (dst_frames == 0) { *out_frames = 0; return nullptr; }

    int16_t *dst = (int16_t *)heap_caps_malloc(dst_frames * 2 * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    if (!dst) { *out_frames = 0; return nullptr; }

    double ratio = (double)src_frames / (double)dst_frames;
    for (size_t i = 0; i < dst_frames; i++) {
        double pos  = i * ratio;
        size_t idx  = (size_t)pos;
        double frac = pos - idx;
        if (idx + 1 >= src_frames) idx = src_frames > 1 ? src_frames - 2 : 0;

        int32_t s0, s1;
        if (src_channels == 1) {
            s0 = src[idx];
            s1 = src[idx + 1];
        } else {
            /* take left channel */
            s0 = src[idx * src_channels];
            s1 = src[(idx + 1) * src_channels];
        }
        int16_t sample = (int16_t)(s0 + (int32_t)(frac * (s1 - s0)));
        dst[i * 2]     = sample;  /* L */
        dst[i * 2 + 1] = sample;  /* R */
    }

    *out_frames = dst_frames;
    return dst;
}

/* ── Decode HTTP chunked transfer encoding in-place ────────────── */
static size_t decode_chunked(uint8_t *buf, size_t len)
{
    /* Quick check: if it already starts with RIFF, not chunked */
    if (len >= 4 && memcmp(buf, "RIFF", 4) == 0) return len;

    /* Verify it looks like chunked: hex digits followed by \r\n */
    bool looks_chunked = false;
    for (size_t i = 0; i < len && i < 10; i++) {
        if (buf[i] == '\r' && i > 0) { looks_chunked = true; break; }
        char c = buf[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')))
            break;
    }
    if (!looks_chunked) return len;

    Serial.println("[TTS-GROQ] Decoding chunked transfer encoding");
    size_t out = 0, pos = 0;
    while (pos < len) {
        /* parse chunk size (hex) */
        unsigned long chunk_sz = 0;
        bool got_digit = false;
        while (pos < len && buf[pos] != '\r') {
            char c = buf[pos++];
            got_digit = true;
            chunk_sz <<= 4;
            if (c >= '0' && c <= '9')      chunk_sz += c - '0';
            else if (c >= 'a' && c <= 'f') chunk_sz += c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') chunk_sz += c - 'A' + 10;
            else { chunk_sz >>= 4; break; }  /* non-hex = stop */
        }
        /* skip \r\n */
        if (pos < len && buf[pos] == '\r') pos++;
        if (pos < len && buf[pos] == '\n') pos++;

        if (chunk_sz == 0 && got_digit) break;  /* final chunk */
        if (chunk_sz == 0) break;                /* safety */

        /* copy chunk data (in-place, always moves backward) */
        size_t to_copy = chunk_sz;
        if (pos + to_copy > len) to_copy = len - pos;
        memmove(buf + out, buf + pos, to_copy);
        out += to_copy;
        pos += chunk_sz;

        /* skip trailing \r\n after chunk data */
        if (pos < len && buf[pos] == '\r') pos++;
        if (pos < len && buf[pos] == '\n') pos++;
    }
    Serial.printf("[TTS-GROQ] Decoded %u → %u bytes\n", (unsigned)len, (unsigned)out);
    return out;
}

/* ── Public API ────────────────────────────────────────────────── */

void tts_groq_init(const String &api_url, const String &api_key)
{
    cfg_url = api_url;
    cfg_key = api_key;
    s_i2s = mic_recorder_get_i2s();
    Serial.printf("[TTS-GROQ] Init (voice=%s i2s=%p)\n",
                  cfg_voice.c_str(), s_i2s);
}

void tts_groq_set_voice(const String &voice)
{
    if (voice.length() > 0) cfg_voice = voice;
    Serial.printf("[TTS-GROQ] Voice set to: %s\n", cfg_voice.c_str());
}

void tts_groq_set_model(const String &model)
{
    if (model.length() > 0) cfg_model = model;
    Serial.printf("[TTS-GROQ] Model set to: %s\n", cfg_model.c_str());
}

void tts_groq_say(const char *text)
{
    if (!s_i2s || !s_i2s->txChan() || !text || !text[0]) return;

    /* Truncate to 200 chars (Groq API limit) */
    char clean[204];
    int j = 0;
    for (int i = 0; text[i] && j < 200; i++) {
        char c = text[i];
        if (c == '\n') c = ' ';
        clean[j++] = c;
    }
    clean[j] = '\0';
    if (j == 0) return;

    Serial.printf("[TTS-GROQ] Say: \"%s\" (voice=%s)\n", clean, cfg_voice.c_str());

    /* Build JSON body */
    JsonDocument doc;
    doc["model"] = cfg_model;
    doc["input"] = clean;
    doc["voice"] = cfg_voice;
    doc["response_format"] = "wav";

    String body;
    serializeJson(doc, body);

    /* HTTPS request */
    WiFiClientSecure *client = new WiFiClientSecure();
    if (!client) {
        Serial.println("[TTS-GROQ] WiFiClientSecure alloc failed");
        return;
    }
    client->setInsecure();

    size_t total_read = 0;
    uint8_t *wav_buf = NULL;
    int content_len = 0;

    /* Scope the HTTPClient so its destructor runs before delete client */
    {
        HTTPClient http;
        String url = cfg_url;
        if (!url.endsWith("/")) url += "/";
        url += "audio/speech";
        Serial.printf("[TTS-GROQ] POST %s\n", url.c_str());

        if (!http.begin(*client, url)) {
            Serial.println("[TTS-GROQ] http.begin() failed");
            delete client;
            return;
        }

        http.addHeader("Content-Type", "application/json");
        if (cfg_key.length() > 0)
            http.addHeader("Authorization", "Bearer " + cfg_key);
        http.setTimeout(15000);

        unsigned long t0 = millis();
        int code = http.sendRequest("POST", body);
        unsigned long elapsed = millis() - t0;
        Serial.printf("[TTS-GROQ] HTTP %d (%lu ms)\n", code, elapsed);

        if (code != 200) {
            String errBody = http.getString();
            Serial.printf("[TTS-GROQ] Error: %s\n", errBody.c_str());
            http.end();
            delete client;
            return;
        }

        /* Read WAV response into PSRAM */
        content_len = http.getSize();
        Serial.printf("[TTS-GROQ] Response size: %d bytes\n", content_len);

        /* Stream into PSRAM buffer */
        size_t alloc_size = content_len > 0 ? (size_t)content_len : 256 * 1024;
        wav_buf = (uint8_t *)heap_caps_malloc(alloc_size, MALLOC_CAP_SPIRAM);
        if (!wav_buf) {
            Serial.println("[TTS-GROQ] PSRAM alloc failed for WAV");
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
                /* timeout: if no data for a while on chunked response, assume done */
                if (content_len < 0) {
                    unsigned long idle_ms = millis() - last_data_ms;
                    if ((total_read > 1000 && idle_ms > 500) || idle_ms > 3000) {
                        Serial.println("[TTS-GROQ] Chunked read timeout – assuming complete");
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
            if (total_read + got > alloc_size) break;  /* safety */
            memcpy(wav_buf + total_read, tmp, got);
            total_read += got;
        }
        http.end();
    }
    /* HTTPClient destroyed — now safe to free the client */
    delete client;

    Serial.printf("[TTS-GROQ] Downloaded %u bytes WAV\n", (unsigned)total_read);

    /* Handle chunked transfer encoding that getStreamPtr() doesn't decode */
    if (content_len < 0 && total_read > 0) {
        total_read = decode_chunked(wav_buf, total_read);
    }

    /* Parse WAV */
    WavInfo wi = {};
    if (!parse_wav(wav_buf, total_read, &wi)) {
        Serial.println("[TTS-GROQ] WAV parse failed");
        heap_caps_free(wav_buf);
        return;
    }

    Serial.printf("[TTS-GROQ] WAV: %uHz %dch %dbit, %u bytes audio\n",
                  wi.sample_rate, wi.channels, wi.bits_per_sample,
                  (unsigned)wi.data_len);

    if (wi.bits_per_sample != 16) {
        Serial.println("[TTS-GROQ] Only 16-bit PCM supported");
        heap_caps_free(wav_buf);
        return;
    }

    const int16_t *pcm = (const int16_t *)wi.data;
    size_t src_frames = wi.data_len / (wi.channels * sizeof(int16_t));

    /* If already 16kHz stereo, write directly */
    if (wi.sample_rate == 16000 && wi.channels == 2) {
        size_t bytes = wi.data_len;
        Serial.printf("[TTS-GROQ] Playing %u bytes directly\n", (unsigned)bytes);
        /* Write in small chunks to avoid starving watchdog */
        size_t off = 0;
        const size_t CHK = 4096;
        while (off < bytes) {
            size_t n = bytes - off;
            if (n > CHK) n = CHK;
            s_i2s->write(wi.data + off, n);
            off += n;
        }
        heap_caps_free(wav_buf);
        return;
    }

    /* Resample to 16kHz stereo */
    size_t out_frames = 0;
    int16_t *resampled = resample_to_16k_stereo(pcm, src_frames,
                                                  wi.channels, wi.sample_rate,
                                                  &out_frames);
    heap_caps_free(wav_buf);

    if (!resampled || out_frames == 0) {
        Serial.println("[TTS-GROQ] Resample failed");
        return;
    }

    size_t bytes = out_frames * 2 * sizeof(int16_t);
    Serial.printf("[TTS-GROQ] Playing %u bytes (%.1f ms at 16kHz)\n",
                  (unsigned)bytes, (float)out_frames * 1000.0f / 16000.0f);

    /* Write in small chunks to avoid starving watchdog */
    size_t offset = 0;
    const size_t CHUNK = 4096;
    while (offset < bytes) {
        size_t n = bytes - offset;
        if (n > CHUNK) n = CHUNK;
        s_i2s->write((uint8_t *)resampled + offset, n);
        offset += n;
    }
    heap_caps_free(resampled);
}
