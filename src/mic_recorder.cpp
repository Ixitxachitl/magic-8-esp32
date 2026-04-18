/*
 *  mic_recorder.cpp – I2S recording from ES8311 ADC + energy-based VAD
 *
 *  Records 16 kHz mono into a PSRAM buffer (max 15 s).
 *  Speech start/end detected by RMS amplitude + silence timeout.
 *
 *  Uses ESP_I2S.h (new Arduino I2S API) matching the official Waveshare
 *  07_ES8311 echo example for this board.  The ES8311 is a full codec
 *  with both DAC (speaker) and ADC (microphone).
 */
#include "mic_recorder.h"
#include "pin_config.h"
#include "es8311.h"
#include "es7210.h"

#include <Arduino.h>
#include <Wire.h>
#include "ESP_I2S.h"
#include <math.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define ES8311_MIC_GAIN_SETTING  ((es8311_mic_gain_t)3)   /* ~18 dB */
#define ES8311_VOLUME            85                        /* 0-100  */

/* ── constants ─────────────────────────────────────────────────── */
#define SAMPLE_RATE         16000
#define MAX_RECORD_SEC      15
#define MAX_SAMPLES         (SAMPLE_RATE * MAX_RECORD_SEC)

/* VAD tuning */
#define VAD_WINDOW_MS       100
#define VAD_WINDOW_SAMPLES  (SAMPLE_RATE * VAD_WINDOW_MS / 1000)   /* 1600 */
#define VAD_SPEECH_THRESH   60        /* RMS threshold for speech           */
#define VAD_SILENCE_MS      3000      /* 3 s silence → end of speech        */
#define VAD_MIN_SPEECH_MS   400       /* at least 0.4 s before allowing end */
#define VAD_LOG_INTERVAL_MS 500       /* log RMS every 500 ms for debug     */

/* ── state ─────────────────────────────────────────────────────── */
static I2SClass        i2s_mic;
static es8311_handle_t s_es_handle         = NULL;
static int16_t       *audio_buffer         = NULL;
static volatile size_t sample_count        = 0;
static volatile bool   recording           = false;
static volatile bool   speech_ended_flag   = false;
static volatile int    audio_level         = 0;
static TaskHandle_t    record_task_handle  = NULL;

/* ── recording task (core 0) ───────────────────────────────────── */
static void record_task(void *param)
{
    (void)param;

    /* Read buffer: stereo interleaved (L+R) */
    const size_t chunk_frames = VAD_WINDOW_SAMPLES;
    const size_t chunk_bytes  = chunk_frames * 2 * sizeof(int16_t);   /* 2 ch */
    int16_t *chunk = (int16_t *)malloc(chunk_bytes);
    if (!chunk) {
        Serial.println("[MIC] Failed to alloc chunk buffer");
        recording = false;
        vTaskDelete(NULL);
        return;
    }

    bool          speech_active   = false;
    unsigned long last_speech_ms  = 0;
    unsigned long first_speech_ms = 0;
    unsigned long last_log_ms     = 0;

    Serial.println("[MIC] Recording task running");

    while (recording && sample_count < MAX_SAMPLES) {
        size_t bytes_read = i2s_mic.readBytes((char *)chunk, chunk_bytes);
        if (bytes_read == 0) {
            vTaskDelay(pdMS_TO_TICKS(1));
            continue;
        }

        size_t frames = bytes_read / (2 * sizeof(int16_t));

        /* Take channel 0 (left) as mono and store */
        int64_t energy = 0;
        size_t sc = sample_count;
        for (size_t i = 0; i < frames && sc < MAX_SAMPLES; i++) {
            int16_t sample = chunk[i * 2];   /* channel 0 */
            audio_buffer[sc++] = sample;
            energy += (int64_t)sample * sample;
        }
        sample_count = sc;

        /* RMS & VAD */
        if (frames > 0) {
            int rms = (int)sqrt((double)energy / frames);
            audio_level = (rms * 100 / 8000);
            if (audio_level > 100) audio_level = 100;

            unsigned long now = millis();

            /* periodic debug logging */
            if (now - last_log_ms > VAD_LOG_INTERVAL_MS) {
                Serial.printf("[MIC] RMS=%d  level=%d  speech=%d  samples=%u\n",
                              rms, audio_level, speech_active, (unsigned)sc);
                last_log_ms = now;
            }

            if (rms > VAD_SPEECH_THRESH) {
                if (!speech_active) {
                    first_speech_ms = now;
                    Serial.printf("[MIC] Speech started (RMS=%d)\n", rms);
                }
                speech_active  = true;
                last_speech_ms = now;
            }

            if (speech_active &&
                now - last_speech_ms > VAD_SILENCE_MS &&
                now - first_speech_ms > VAD_MIN_SPEECH_MS) {
                Serial.printf("[MIC] Speech ended (silence %lu ms)\n",
                              now - last_speech_ms);
                speech_ended_flag = true;
                break;
            }
        }
    }

    if (sample_count >= MAX_SAMPLES) {
        Serial.println("[MIC] Max recording length reached");
        speech_ended_flag = true;
    }

    recording   = false;
    audio_level = 0;
    free(chunk);
    Serial.printf("[MIC] Stopped: %u samples (%.1f s)\n",
                  (unsigned)sample_count,
                  (float)sample_count / SAMPLE_RATE);
    record_task_handle = NULL;
    vTaskDelete(NULL);
}

/* ── public API ────────────────────────────────────────────────── */

bool mic_recorder_init(void)
{
    Serial.println("[MIC] Initializing I2S + ES8311 codec (ESP_I2S.h, STD mode)...");

    /* -- I2S via ESP_I2S.h (matching Waveshare 07_ES8311 echo example) ──── */
    i2s_mic.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
    if (!i2s_mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("[MIC] I2S init FAILED!");
        return false;
    }
    Serial.printf("[MIC] I2S started: BCK=%d WS=%d DOUT=%d DIN=%d MCK=%d (ESP_I2S STD mode)\n",
                  I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);

    /* Let MCLK stabilize */
    delay(100);

    /* PA HIGH before codec init – matching Waveshare 07_ES8311 example order */
    pinMode(PA, OUTPUT);
    digitalWrite(PA, HIGH);

    /* -- ES8311 codec init (provides both speaker DAC + microphone ADC) ── */
    if (!s_es_handle) {
        s_es_handle = es8311_create(0, ES8311_ADDRESS_0);
    }
    if (!s_es_handle) {
        Serial.println("[MIC] ES8311 create FAILED");
        return false;
    }

    const es8311_clock_config_t es_clk = {
        .mclk_inverted    = false,
        .sclk_inverted    = false,
        .mclk_from_mclk_pin = true,
        .mclk_frequency   = SAMPLE_RATE * 256,   /* 4.096 MHz */
        .sample_frequency = SAMPLE_RATE
    };

    esp_err_t ret = es8311_init(s_es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 init FAILED: 0x%x\n", ret);
        return false;
    }

    ret = es8311_sample_frequency_config(s_es_handle, es_clk.mclk_frequency, es_clk.sample_frequency);
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 sample freq config FAILED: 0x%x\n", ret);
    }

    ret = es8311_microphone_config(s_es_handle, false);   /* analog mic */
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 mic config FAILED: 0x%x\n", ret);
    }

    ret = es8311_voice_volume_set(s_es_handle, ES8311_VOLUME, NULL);
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 volume set FAILED: 0x%x\n", ret);
    }

    /* UNMUTE the DAC – REG31 defaults to 0x60 (both channels muted!) */
    ret = es8311_voice_mute(s_es_handle, false);
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 unmute FAILED: 0x%x\n", ret);
    }

    ret = es8311_microphone_gain_set(s_es_handle, ES8311_MIC_GAIN_SETTING);
    if (ret != ESP_OK) {
        Serial.printf("[MIC] ES8311 mic gain set FAILED: 0x%x\n", ret);
    }

    Serial.println("[MIC] ES8311 codec init OK (mic + speaker)");

    /* -- ES7210 quad-channel mic ADC (dual microphone array) ────────────── */
    {
        /* Verify ES7210 is present on I2C bus */
        Wire.beginTransmission(0x42);  /* ES7210_AD1_AD0_10 */
        uint8_t i2c_err = Wire.endTransmission();
        if (i2c_err == 0) {
            Serial.println("[MIC] ES7210 found at 0x42 – initializing...");
            audio_hal_codec_config_t cfg = {};
            esp_err_t es7210_ret = es7210_adc_init(&Wire, &cfg);
            if (es7210_ret == ESP_OK) {
                Serial.println("[MIC] ES7210 init OK (dual mic ADC)");
            } else {
                Serial.printf("[MIC] ES7210 init FAILED: 0x%x\n", es7210_ret);
            }
        } else {
            Serial.printf("[MIC] ES7210 NOT found at 0x42 (i2c err=%d)\n", i2c_err);
        }
    }
    {
        const size_t test_bytes = 64 * 2 * sizeof(int16_t);
        int16_t *test = (int16_t *)malloc(test_bytes);
        if (test) {
            delay(50);   /* let ADC settle */
            size_t br = i2s_mic.readBytes((char *)test, test_bytes);
            int nonzero = 0;
            for (size_t i = 0; i < br / sizeof(int16_t); i++) {
                if (test[i] != 0) nonzero++;
            }
            Serial.printf("[MIC] Boot mic test: %u bytes read, %d/%u non-zero samples\n",
                          (unsigned)br, nonzero, (unsigned)(br / sizeof(int16_t)));
            if (br >= 8 * sizeof(int16_t)) {
                for (int i = 0; i < 4; i++) {
                    Serial.printf("[MIC]   frame[%d] L=%d R=%d\n", i, test[i*2], test[i*2+1]);
                }
            }
            free(test);
        }
    }

    /* -- PSRAM audio buffer ──────────────────────────────────────────────── */
    if (!audio_buffer) {
        audio_buffer = (int16_t *)heap_caps_malloc(
            MAX_SAMPLES * sizeof(int16_t), MALLOC_CAP_SPIRAM);
        if (!audio_buffer) {
            Serial.println("[MIC] PSRAM buffer alloc failed");
            return false;
        }
    }
    Serial.printf("[MIC] Init OK  buffer=%uKB PSRAM\n",
                  (unsigned)(MAX_SAMPLES * sizeof(int16_t) / 1024));

    return true;
}

bool mic_recorder_start(void)
{
    if (recording) return false;

    sample_count      = 0;
    speech_ended_flag = false;
    audio_level       = 0;
    recording         = true;

    xTaskCreatePinnedToCore(record_task, "mic_rec", 4096,
                            NULL, 2, &record_task_handle, 0);
    Serial.println("[MIC] Recording started");
    return true;
}

void mic_recorder_stop(void)
{
    recording = false;
    if (record_task_handle) {
        vTaskDelay(pdMS_TO_TICKS(150));
        record_task_handle = NULL;
    }
    Serial.println("[MIC] Recording stopped (manual)");
}

bool         mic_recorder_is_recording(void)    { return recording; }
bool         mic_recorder_speech_ended(void)    { return speech_ended_flag; }
int          mic_recorder_get_audio_level(void) { return audio_level; }
const int16_t *mic_recorder_get_audio_buffer(void) { return audio_buffer; }
size_t       mic_recorder_get_sample_count(void) { return sample_count; }

I2SClass *mic_recorder_get_i2s(void) { return &i2s_mic; }

void mic_recorder_suspend_i2s(void)
{
    if (recording) {
        recording = false;
        vTaskDelay(pdMS_TO_TICKS(150));
        record_task_handle = NULL;
    }
    i2s_mic.end();
    Serial.println("[MIC] I2S suspended for speaker");
}

void mic_recorder_resume_i2s(void)
{
    i2s_mic.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
    if (!i2s_mic.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                       I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
        Serial.println("[MIC] I2S resume FAILED!");
        return;
    }
    delay(100);

    /* Re-init ES8311 for microphone recording */
    if (s_es_handle) {
        const es8311_clock_config_t es_clk = {
            .mclk_inverted      = false,
            .sclk_inverted      = false,
            .mclk_from_mclk_pin = true,
            .mclk_frequency     = SAMPLE_RATE * 256,
            .sample_frequency   = SAMPLE_RATE
        };
        es8311_init(s_es_handle, &es_clk, ES8311_RESOLUTION_16, ES8311_RESOLUTION_16);
        es8311_sample_frequency_config(s_es_handle, es_clk.mclk_frequency, es_clk.sample_frequency);
        es8311_microphone_config(s_es_handle, false);
        es8311_voice_volume_set(s_es_handle, ES8311_VOLUME, NULL);
        es8311_microphone_gain_set(s_es_handle, ES8311_MIC_GAIN_SETTING);
    }
    Serial.println("[MIC] I2S resumed for mic");
}
