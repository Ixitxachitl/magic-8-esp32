/*
 *  tone_player.cpp – Feedback sounds via ES8311 DAC (I2S)
 *
 *  The official Waveshare 07_ES8311 example proves that i2s.write()
 *  works with the standard es8311_init() driver — NO extra register
 *  writes needed.  Just set PA HIGH and write PCM.
 *
 *  Pre-generates tone buffers in PSRAM (like the pocket-watch project)
 *  and writes them in one large i2s.write() call.
 */
#include "tone_player.h"
#include "mic_recorder.h"
#include "pin_config.h"
#include <Arduino.h>
#include "ESP_I2S.h"
#include <math.h>
#include <esp_heap_caps.h>

#define TONE_SR 16000

static I2SClass *_i2s = nullptr;

/* Pre-generated stereo PCM buffers (PSRAM) */
static int16_t *beep_listen_pcm = nullptr;
static int      beep_listen_len = 0;   /* total int16_t count (stereo) */
static int16_t *beep_done_pcm   = nullptr;
static int      beep_done_len   = 0;
static int16_t *chime_pcm       = nullptr;
static int      chime_len       = 0;

/*
 * Generate a sine tone with envelope into a stereo buffer.
 * Returns number of STEREO SAMPLES written (= mono_samples, since each
 * mono sample produces L+R).
 */
static int generate_tone(int16_t *buf, int offset, float freq, int ms, float vol)
{
    int mono_samples = TONE_SR * ms / 1000;
    for (int i = 0; i < mono_samples; i++) {
        float pos = (float)i / mono_samples;
        float env = 1.0f;
        if (pos < 0.15f) env = pos / 0.15f;          /* slow attack  */
        if (pos > 0.50f) env = (1.0f - pos) / 0.50f;  /* long fade    */

        float t = (float)i / TONE_SR;
        float sample = sinf(2.0f * M_PI * freq * t) * vol * env;
        sample += sinf(2.0f * M_PI * freq * 2.0f * t) * vol * env * 0.15f;
        int16_t s = (int16_t)(sample * 30000.0f);
        buf[offset + i * 2]     = s;   /* L */
        buf[offset + i * 2 + 1] = s;   /* R */
    }
    return mono_samples;
}

/* Generate silence (stereo) */
static int generate_silence(int16_t *buf, int offset, int ms)
{
    int mono_samples = TONE_SR * ms / 1000;
    memset(&buf[offset], 0, mono_samples * 2 * sizeof(int16_t));
    return mono_samples;
}

/* Allocate PSRAM and fill with a sequence of tones+gaps */
typedef struct { float freq; int ms; float vol; } tone_step_t;

static int16_t *build_sequence(const tone_step_t *steps, int count, int *out_len)
{
    /* Calculate total mono samples */
    int total_mono = 0;
    for (int i = 0; i < count; i++)
        total_mono += TONE_SR * steps[i].ms / 1000;

    /* Stereo: 2 int16_t per mono sample */
    int total_stereo = total_mono * 2;
    int16_t *buf = (int16_t *)heap_caps_malloc(total_stereo * sizeof(int16_t),
                                                MALLOC_CAP_SPIRAM);
    if (!buf) {
        Serial.println("[TONE] PSRAM alloc failed!");
        *out_len = 0;
        return nullptr;
    }

    int offset = 0;
    for (int i = 0; i < count; i++) {
        int mono;
        if (steps[i].freq == 0)
            mono = generate_silence(buf, offset, steps[i].ms);
        else
            mono = generate_tone(buf, offset, steps[i].freq, steps[i].ms, steps[i].vol);
        offset += mono * 2;
    }

    *out_len = total_stereo;
    return buf;
}

/* ── public API ────────────────────────────────────────────────── */

void tone_player_init(void)
{
    _i2s = mic_recorder_get_i2s();
    pinMode(PA, OUTPUT);
    digitalWrite(PA, HIGH);

    /* Verify TX channel is alive */
    Serial.printf("[TONE] i2s=%p  txChan=%p  PA=%d(HIGH)\n",
                  _i2s, _i2s ? (void*)_i2s->txChan() : nullptr, PA);
    if (!_i2s || !_i2s->txChan()) {
        Serial.println("[TONE] *** FATAL: TX channel is NULL – cannot play audio! ***");
        return;
    }

    /* Build PCM buffers in PSRAM */
    {
        const tone_step_t steps[] = {
            {660, 60, 0.18f}, {0, 30, 0}, {880, 50, 0.15f}, {0, 40, 0}
        };
        beep_listen_pcm = build_sequence(steps, 4, &beep_listen_len);
    }
    {
        const tone_step_t steps[] = {
            {880, 50, 0.15f}, {0, 30, 0}, {660, 60, 0.12f}, {0, 40, 0}
        };
        beep_done_pcm = build_sequence(steps, 4, &beep_done_len);
    }
    {
        const tone_step_t steps[] = {
            {392, 120, 0.14f}, {0, 50, 0},
            {523, 120, 0.16f}, {0, 50, 0},
            {659, 180, 0.18f}, {0, 80, 0}
        };
        chime_pcm = build_sequence(steps, 6, &chime_len);
    }

    Serial.printf("[TONE] init OK (i2s=%p PA=%d)\n", _i2s, PA);
}

void tone_player_beep_listen(void)
{
    if (!_i2s || !beep_listen_pcm) return;
    Serial.println("[TONE] beep_listen");
    _i2s->write((uint8_t *)beep_listen_pcm,
                beep_listen_len * sizeof(int16_t));
}

void tone_player_beep_done(void)
{
    if (!_i2s || !beep_done_pcm) return;
    Serial.println("[TONE] beep_done");
    _i2s->write((uint8_t *)beep_done_pcm,
                beep_done_len * sizeof(int16_t));
}

void tone_player_chime(void)
{
    if (!_i2s || !chime_pcm) return;
    Serial.println("[TONE] chime");
    _i2s->write((uint8_t *)chime_pcm,
                chime_len * sizeof(int16_t));
}
