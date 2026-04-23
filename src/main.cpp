/*
 *  main.cpp – Magic 8 Ball
 *  Target: Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *          CO5300 466×466 QSPI AMOLED
 *
 *  Tap the ball → asks a free LLM for a mystical
 *  one-line answer displayed inside a classic blue-triangle window.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <lvgl.h>
#include <math.h>
#include "Arduino_GFX_Library.h"
#include <TouchDrvCSTXXX.hpp>
#define XPOWERS_CHIP_AXP2101
#include <XPowersAXP2101.tpp>

#include "pin_config.h"
#include "magic8ball_ui.h"
#include "llm_client.h"
#include "config_portal.h"
#include "mic_recorder.h"
#include "stt_client.h"
#include "tone_player.h"
#include "tts_sam.h"
#include "tts_groq.h"
#include "tts_elevenlabs.h"
#include <esp_task_wdt.h>

/* ── Classic Magic 8 Ball fallback answers (no-WiFi mode) ──────── */
static const char *CLASSIC[] = {
    "It is certain",        "It is\ndecidedly so",
    "Without\na doubt",     "Yes\ndefinitely",
    "You may\nrely on it",  "As I see it\nyes",
    "Most likely",           "Outlook good",
    "Yes",                   "Signs\npoint to yes",
    "Reply hazy\ntry again", "Ask again\nlater",
    "Better not\ntell you now","Cannot\npredict now",
    "Concentrate\nand ask again",
    "Don't\ncount on it",   "My reply\nis no",
    "My sources\nsay no",   "Outlook\nnot so good",
    "Very\ndoubtful"
};
static const int NUM_CLASSIC = sizeof(CLASSIC) / sizeof(CLASSIC[0]);

/* ── Display driver ────────────────────────────────────────────── */
Arduino_DataBus *bus = new Arduino_ESP32QSPI(
    LCD_CS, LCD_SCLK,
    LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);

Arduino_CO5300 *gfx = new Arduino_CO5300(
    bus, LCD_RESET, 0 /* rotation */,
    LCD_WIDTH, LCD_HEIGHT,
    6 /* col_offset */, 0, 0, 0);

/* ── LVGL buffers ──────────────────────────────────────────────── */
static const uint32_t LV_BUF_SIZE    = LCD_WIDTH * LCD_HEIGHT;
static const uint32_t STAGING_LINES  = 120;
static lv_disp_draw_buf_t draw_buf;
static lv_color_t *buf1    = nullptr;
static lv_color_t *staging = nullptr;

static void my_disp_flush(lv_disp_drv_t *drv, const lv_area_t *area,
                           lv_color_t *color_p)
{
    uint32_t w = area->x2 - area->x1 + 1;
    uint32_t h = area->y2 - area->y1 + 1;

    gfx->startWrite();
    lv_coord_t y = area->y1;
    uint32_t rows_left = h;
    while (rows_left > 0) {
        uint32_t chunk_h  = rows_left > STAGING_LINES ? STAGING_LINES : rows_left;
        uint32_t chunk_px = w * chunk_h;
        if (w == LCD_WIDTH) {
            memcpy(staging,
                   (uint16_t *)color_p + (uint32_t)y * LCD_WIDTH,
                   chunk_px * sizeof(uint16_t));
        } else {
            uint16_t *dst = (uint16_t *)staging;
            for (uint32_t r = 0; r < chunk_h; r++) {
                memcpy(dst,
                       (uint16_t *)color_p + ((uint32_t)(y + r) * LCD_WIDTH + area->x1),
                       w * sizeof(uint16_t));
                dst += w;
            }
        }
#if (LV_COLOR_16_SWAP != 0)
        gfx->draw16bitBeRGBBitmap(area->x1, y, (uint16_t *)staging, w, chunk_h);
#else
        gfx->draw16bitRGBBitmap(area->x1, y, (uint16_t *)staging, w, chunk_h);
#endif
        y         += chunk_h;
        rows_left -= chunk_h;
    }
    gfx->endWrite();
    lv_disp_flush_ready(drv);
}

/* ── Touch ─────────────────────────────────────────────────────── */
TouchDrvCST92xx touch;
static bool touch_ok = false;

/* I2C bus mutex – protects Wire access across cores */
static SemaphoreHandle_t i2c_mux = NULL;

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int16_t x[1], y[1];
    bool pressed = false;
    if (touch_ok && i2c_mux) {
        if (xSemaphoreTake(i2c_mux, pdMS_TO_TICKS(5))) {
            pressed = touch.getPoint(x, y, 1);
            xSemaphoreGive(i2c_mux);
        }
    }
    if (pressed) {
        data->point.x = (LCD_WIDTH  - 1) - x[0];
        data->point.y = (LCD_HEIGHT - 1) - y[0];
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* ── Power button double-press detection ───────────────────────── */
#define DOUBLE_PRESS_WINDOW_MS 600
static unsigned long last_pkey_ms     = 0;
static int           pkey_press_count = 0;
static bool          pkey_single_pending = false;   /* waiting for double-press window */
static unsigned long batt_display_ms    = 0;        /* when battery % was shown        */
static bool          showing_battery    = false;

/* ── PMIC ──────────────────────────────────────────────────────── */
XPowersAXP2101 pmic;
static bool pmic_ok = false;

/* ── LVGL rendering task (runs on core 0 for smooth animation) ── */
static SemaphoreHandle_t lvgl_mux = NULL;

static void lvgl_task(void *param)
{
    (void)param;
    for (;;) {
        if (xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY)) {
            lv_timer_handler();
            xSemaphoreGiveRecursive(lvgl_mux);
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

static inline void lvgl_lock(void)   { xSemaphoreTakeRecursive(lvgl_mux, portMAX_DELAY); }
static inline void lvgl_unlock(void) { xSemaphoreGiveRecursive(lvgl_mux); }

/* ── Ask the 8 Ball ────────────────────────────────────────────── */
static bool use_llm = false;
static bool mic_ok  = false;
static String tts_mode_str;  /* "sam", "groq", "off" */

enum AppState {
    STATE_IDLE,
    STATE_LISTENING,              /* recording + live transcription   */
    STATE_TRANSCRIBING,           /* final STT after speech ended     */
    STATE_SETTLING_TRANSCRIPT,    /* decelerating spin before text    */
    STATE_SHOWING_TRANSCRIPT,     /* transcript visible, then re-spin */
    STATE_ASKING_LLM,             /* waiting for 8-ball answer        */
    STATE_SETTLING_ANSWER         /* decelerating spin before answer  */
};
static AppState       app_state          = STATE_IDLE;
static unsigned long  last_partial_ms    = 0;
static unsigned long  transcript_shown_ms = 0;
static String         final_transcript;
static String         pending_answer;
static bool           settle_complete      = false;

/* ── TTS background task ───────────────────────────────────────── */
static volatile bool  tts_playing          = false;
static char          *tts_task_buf         = NULL;

static void tts_bg_task(void *param)
{
    (void)param;
    if (tts_task_buf) {
        if (tts_mode_str == "groq" || tts_mode_str == "openai") {
            tts_groq_say(tts_task_buf);
        } else if (tts_mode_str == "elevenlabs") {
            tts_elevenlabs_say(tts_task_buf);
        } else if (tts_mode_str == "sam") {
            tts_say(tts_task_buf);
        }
        free(tts_task_buf);
        tts_task_buf = NULL;
    }
    tts_playing = false;
    vTaskDelete(NULL);
}

/* ── Apply all provider/key settings live ──────────────────────── */
static void apply_provider_settings()
{
    /* LLM */
    String key = config_portal_get_api_key();
    if (key.length() > 0) {
        llm_init(config_portal_get_api_url(), key, config_portal_get_model());
        llm_set_system_prompt(config_portal_get_system_prompt());
        use_llm = true;
        Serial.println("[SETTINGS] LLM re-init OK");
    } else {
        use_llm = false;
        Serial.println("[SETTINGS] LLM disabled (no key)");
    }

    /* STT */
    if (mic_ok) {
        String stt_url = config_portal_get_stt_url();
        String stt_key = config_portal_get_stt_key();
        if (stt_key.length() > 0 && stt_url.length() > 0) {
            stt_init(stt_url, stt_key, config_portal_get_stt_model());
            Serial.printf("[SETTINGS] STT: %s / %s\n",
                          config_portal_get_stt_provider().c_str(),
                          config_portal_get_stt_model().c_str());
        } else {
            Serial.println("[SETTINGS] STT disabled (no key)");
        }
    }

    /* TTS */
    if (mic_ok) {
        tts_mode_str = config_portal_get_tts_mode();
        if (tts_mode_str == "groq" || tts_mode_str == "openai") {
            String tts_key = config_portal_get_tts_key();
            if (tts_key.length() > 0) {
                tts_groq_init(config_portal_get_tts_url(), tts_key);
                tts_groq_set_voice(config_portal_get_tts_voice());
                if (tts_mode_str == "openai") {
                    tts_groq_set_model("tts-1");
                    Serial.println("[SETTINGS] TTS: OpenAI");
                } else {
                    tts_groq_set_model("canopylabs/orpheus-v1-english");
                    Serial.println("[SETTINGS] TTS: Groq Orpheus");
                }
            } else {
                tts_mode_str = "sam";
                tts_init();
                Serial.println("[SETTINGS] TTS: key missing, fallback SAM");
            }
        } else if (tts_mode_str == "elevenlabs") {
            String tts_key = config_portal_get_tts_key();
            if (tts_key.length() > 0) {
                tts_elevenlabs_init(tts_key);
                tts_elevenlabs_set_voice(config_portal_get_tts_voice());
                Serial.println("[SETTINGS] TTS: ElevenLabs");
            } else {
                tts_mode_str = "sam";
                tts_init();
                Serial.println("[SETTINGS] TTS: ElevenLabs key missing, fallback SAM");
            }
        } else if (tts_mode_str == "sam") {
            tts_init();
            Serial.println("[SETTINGS] TTS: SAM");
        } else {
            Serial.println("[SETTINGS] TTS: Off");
        }
    }
}

static void on_settings_changed()
{
    Serial.println("[SETTINGS] Portal settings changed – applying live...");
    apply_provider_settings();
}

static void on_settle_done(void) { settle_complete = true; }

static void on_ask(void)
{
    if (app_state != STATE_IDLE || tts_playing) {
        Serial.println("[MAIN] on_ask: busy, ignoring");
        return;
    }

    Serial.println("[MAIN] on_ask: Asking the 8 Ball...");
    lvgl_lock();
    magic8ball_ui_start_anim();

    lvgl_unlock();

    /* If mic + LLM available → voice flow */
    if (use_llm && mic_ok) {
        tone_player_beep_listen();
        mic_recorder_start();
        /* Drain any stale STT result from a previous session */
        { String discard; while (stt_check_result(discard)) {} }
        /* Drain any stale LLM result too */
        { String discard; while (llm_check_result(discard)) {} }
        lvgl_lock();
        magic8ball_ui_set_listening(true);
        lvgl_unlock();
        app_state        = STATE_LISTENING;
        last_partial_ms  = millis();
        final_transcript = "";   /* clear for fresh capture */
        Serial.println("[MAIN] Entered LISTENING state (voice)");
        return;
    }

    /* LLM without mic → direct request */
    if (use_llm) {
        lvgl_lock();
        magic8ball_ui_set_thinking(true);
        lvgl_unlock();
        llm_start_request();
        app_state = STATE_ASKING_LLM;
        Serial.println("[MAIN] Entered ASKING_LLM state (no mic)");
        return;
    }

    /* Offline → classic answer */
    lvgl_lock();
    magic8ball_ui_set_thinking(true);
    lvgl_unlock();
    int idx = random(0, NUM_CLASSIC);
    Serial.printf("[MAIN] Offline, classic answer #%d\n", idx);
    static int chosen;
    chosen = idx;
    lvgl_lock();
    lv_timer_create([](lv_timer_t *t) {
        magic8ball_ui_stop_anim();
        magic8ball_ui_set_answer(CLASSIC[*((int *)t->user_data)]);
        lv_timer_del(t);
    }, 1200, &chosen);
    lvgl_unlock();
    /* stay IDLE – timer callback shows answer */
}

static void on_longpress(void)
{
    if (tts_playing || app_state != STATE_IDLE) return;

    Serial.println("[MAIN] Double-press detected \u2013 entering AP setup");
    lvgl_lock();
    magic8ball_ui_set_answer("Entering\nsetup...");
    lvgl_unlock();
    delay(800);
    config_portal_start_ap();
    lvgl_lock();
    magic8ball_ui_set_answer("Connect WiFi:\nMagic8Ball\n-Setup\n192.168.4.1");
    lvgl_unlock();
}

/* ── Arduino setup ─────────────────────────────────────────────── */
void setup()
{
    Serial.begin(115200);
    delay(200);  /* let USB CDC settle */
    Serial.println();
    Serial.println("==================================");
    Serial.println("  Magic 8 Ball – ESP32-S3 AMOLED");
    Serial.println("==================================");
    Wire.begin(IIC_SDA, IIC_SCL);
    delay(50);
    Serial.printf("[INIT] I2C started (SDA=%d SCL=%d)\n", IIC_SDA, IIC_SCL);

    /* I2C bus mutex – must be created before any I2C peripheral init */
    i2c_mux = xSemaphoreCreateMutex();

    /* ── PMIC (powers other I2C devices) ─────────────────────── */
    for (int attempt = 0; attempt < 3; attempt++) {
        if (pmic.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL)) {
            pmic_ok = true;
            break;
        }
        Serial.printf("[INIT] PMIC attempt %d failed, retrying...\n", attempt + 1);
        delay(100);
    }
    if (pmic_ok) {
        pmic.enableBattDetection();
        pmic.enableBattVoltageMeasure();
        pmic.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
        pmic.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ);
        pmic.clearIrqStatus();

        /* Enable audio power rails */
        pmic.setBLDO2Voltage(3300);  /* ES7210 mic ADC VDD */
        pmic.enableBLDO2();
        pmic.setBLDO1Voltage(3300);  /* ES8311 speaker codec VDD */
        pmic.enableBLDO1();

        /* Force a full power-cycle of the codec to clear any stuck state */
        pmic.disableBLDO1();
        pmic.disableBLDO2();
        delay(200);
        pmic.enableBLDO2();
        pmic.enableBLDO1();
        delay(100);

        Serial.println("[INIT] PMIC AXP2101 OK (BLDO1+BLDO2 enabled for audio)");
        Serial.printf("[INIT]   Battery: %d%% (%.2fV) charging=%d\n",
                      pmic.getBatteryPercent(),
                      pmic.getBattVoltage() / 1000.0f,
                      pmic.isCharging());
    } else {
        Serial.println("[INIT] PMIC AXP2101 NOT FOUND after 3 attempts");
    }

    /* ── Display ─────────────────────────────────────────────── */
    if (!gfx->begin()) {
        Serial.println("[INIT] Display init FAILED – halting");
        while (true) delay(1000);
    }
    gfx->fillScreen(0x0000);
    gfx->setBrightness(200);
    Serial.printf("[INIT] Display OK (%dx%d CO5300 QSPI AMOLED)\n", LCD_WIDTH, LCD_HEIGHT);

    /* ── Touch ───────────────────────────────────────────────── */
    touch.setPins(TP_RST, TP_INT);
    touch_ok = touch.begin(Wire, CST92XX_SLAVE_ADDRESS);
    Serial.printf("[INIT] Touch CST9217 %s\n", touch_ok ? "OK" : "FAIL");

    /* ── LVGL ────────────────────────────────────────────────── */
    lv_init();
    Serial.println("[INIT] LVGL initialized");

    buf1    = (lv_color_t *)heap_caps_malloc(LV_BUF_SIZE * sizeof(lv_color_t),
                                             MALLOC_CAP_SPIRAM);
    staging = (lv_color_t *)heap_caps_malloc(LCD_WIDTH * STAGING_LINES * sizeof(lv_color_t),
                                             MALLOC_CAP_DMA);
    if (!buf1 || !staging) {
        Serial.println("[INIT] LVGL buffer alloc FAILED – halting");
        while (true) delay(1000);
    }
    Serial.printf("[INIT] LVGL buffers: PSRAM=%uKB  DMA staging=%uKB\n",
                  (LV_BUF_SIZE * sizeof(lv_color_t)) / 1024,
                  (LCD_WIDTH * STAGING_LINES * sizeof(lv_color_t)) / 1024);
    lv_disp_draw_buf_init(&draw_buf, buf1, NULL, LV_BUF_SIZE);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res     = LCD_WIDTH;
    disp_drv.ver_res     = LCD_HEIGHT;
    disp_drv.flush_cb    = my_disp_flush;
    disp_drv.draw_buf    = &draw_buf;
    disp_drv.direct_mode = 1;
    lv_disp_drv_register(&disp_drv);
    Serial.println("[INIT] LVGL display driver registered (direct_mode)");

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type    = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = my_touchpad_read;
    lv_indev_drv_register(&indev_drv);
    Serial.println("[INIT] LVGL touch input registered");

    /* ── Magic 8 Ball UI ───────────────────────────────────────── */
    Serial.println("[INIT] Creating Magic 8 Ball UI...");
    magic8ball_ui_init(lv_scr_act(), LCD_WIDTH);
    magic8ball_ui_set_tap_cb(on_ask);
    /* AP mode is triggered by double-pressing the power button, not touch */

    /* first render so the user sees something during WiFi connect */
    magic8ball_ui_set_answer("Connecting\nto WiFi...");
    lv_timer_handler();

    /* ── Start LVGL rendering task on core 0 ──────────────────── */
    lvgl_mux = xSemaphoreCreateRecursiveMutex();
    xTaskCreatePinnedToCore(lvgl_task, "lvgl", 8192, NULL, 3, NULL, 0);
    /* LVGL task owns core 0 and starves IDLE0 during QSPI flush.
       Deinit the task WDT and reinit without idle-task monitoring
       so IDLE0 starvation doesn't trigger a panic. */
    esp_task_wdt_deinit();
    const esp_task_wdt_config_t twdt_cfg = {
        .timeout_ms    = 5000,
        .idle_core_mask = 0,       /* don't subscribe any idle tasks */
        .trigger_panic  = true,
    };
    esp_task_wdt_init(&twdt_cfg);
    Serial.println("[INIT] LVGL rendering task started on core 0");

    /* ── WiFi + config portal ────────────────────────────────── */    Serial.println("[INIT] Starting WiFi / config portal...");    config_portal_begin();
    config_portal_set_settings_changed_cb(on_settings_changed);

    if (config_portal_is_configured()) {
        /* ── Microphone (ES8311) ─────────────────────────────── */
        mic_ok = mic_recorder_init();
        Serial.printf("[INIT] Microphone %s\n", mic_ok ? "OK" : "FAIL (voice disabled)");

        /* ── Tone player (needs mic I2S instance) ────────────── */
        if (mic_ok) {
            tone_player_init();
            Serial.println("[INIT] Tone player OK");
        }

        /* ── Apply all provider settings ────────────────────── */
        apply_provider_settings();

        magic8ball_ui_set_answer("TAP TO\nASK");
    } else {
        Serial.println("[INIT] Not configured – showing AP setup screen");
        magic8ball_ui_set_answer("Connect WiFi:\nMagic8Ball\n-Setup\n192.168.4.1");
    }

    Serial.println("[INIT] ---- Memory Report ----");
    Serial.printf("[INIT]   Free heap:     %u bytes\n", ESP.getFreeHeap());
    Serial.printf("[INIT]   Free internal: %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    Serial.printf("[INIT]   Free PSRAM:    %u bytes\n", heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    Serial.println("[INIT] ---- Setup Complete ----");
}

/* ── Arduino loop ──────────────────────────────────────────────── */
void loop()
{
    /* LVGL is serviced by the dedicated lvgl_task on core 0 */

    /* ── state machine ─────────────────────────────────────────── */
    switch (app_state) {

    case STATE_LISTENING: {
        /* audio-level → triangle glow */
        lvgl_lock();
        magic8ball_ui_set_audio_level(mic_recorder_get_audio_level());
        lvgl_unlock();

        /* speech ended or silence timeout → send full audio to STT */
        bool timed_out = millis() - last_partial_ms > 8000;
        if (mic_recorder_speech_ended() || timed_out) {
            mic_recorder_stop();
            tone_player_beep_done();

            size_t samples = mic_recorder_get_sample_count();
            final_transcript = "";

            if (samples > 1600) {   /* at least 0.1 s of audio */
                Serial.printf("[LOOP] %s → TRANSCRIBING (%u samples)\n",
                              timed_out ? "Timeout with audio" : "Speech ended",
                              (unsigned)samples);
                stt_start_transcribe(mic_recorder_get_audio_buffer(), samples);
                app_state = STATE_TRANSCRIBING;
                transcript_shown_ms = millis();
            } else {
                Serial.println("[LOOP] No usable audio – back to idle");
                lvgl_lock();
                magic8ball_ui_stop_anim();
                magic8ball_ui_set_answer("TAP TO\nASK");
                lvgl_unlock();
                app_state = STATE_IDLE;
            }
        }
        break;
    }

    case STATE_TRANSCRIBING: {
        /* Wait for full STT result */
        {
            String text;
            if (stt_check_result(text)) {
                if (text.length() > 0) {
                    Serial.printf("[LOOP] Final transcript: \"%s\"\n", text.c_str());
                    final_transcript = text;
                    /* Settle spin to show transcript */
                    settle_complete = false;
                    lvgl_lock();
                    magic8ball_ui_settle_then(on_settle_done);
                    lvgl_unlock();
                    app_state = STATE_SETTLING_TRANSCRIPT;
                    Serial.println("[LOOP] → SETTLING_TRANSCRIPT");
                } else {
                    Serial.println("[LOOP] Empty transcript – back to idle");
                    lvgl_lock();
                    magic8ball_ui_stop_anim();
                    magic8ball_ui_set_answer("I didn't\nhear anything");
                    lvgl_unlock();
                    app_state = STATE_IDLE;
                }
            }
        }
        /* Safety timeout: 20s max waiting for STT */
        if (millis() - transcript_shown_ms > 20000) {
            Serial.println("[LOOP] STT timeout – back to idle");
            lvgl_lock();
            magic8ball_ui_stop_anim();
            magic8ball_ui_set_answer("Timed out\ntry again");
            lvgl_unlock();
            app_state = STATE_IDLE;
        }
        break;
    }

    case STATE_SETTLING_TRANSCRIPT: {
        if (settle_complete) {
            lvgl_lock();
            magic8ball_ui_set_transcript(final_transcript.c_str());
            lvgl_unlock();
            transcript_shown_ms = millis();
            app_state = STATE_SHOWING_TRANSCRIPT;
            Serial.println("[LOOP] → SHOWING_TRANSCRIPT");
        }
        break;
    }

    case STATE_SHOWING_TRANSCRIPT: {
        /* Show transcript briefly, then re-spin and send to LLM */
        if (millis() - transcript_shown_ms > 1500) {
            /* Fire LLM request now */
            llm_start_request_with_question(final_transcript);
            lvgl_lock();
            magic8ball_ui_start_anim();
            lvgl_unlock();
            app_state = STATE_ASKING_LLM;
            Serial.println("[LOOP] → ASKING_LLM");
        }
        break;
    }

    case STATE_ASKING_LLM: {
        String answer;
        if (llm_check_result(answer)) {
            Serial.printf("[LOOP] LLM answer: \"%s\"\n", answer.c_str());
            pending_answer = answer;
            settle_complete = false;
            lvgl_lock();
            magic8ball_ui_settle_then(on_settle_done);
            lvgl_unlock();
            app_state = STATE_SETTLING_ANSWER;
            Serial.println("[LOOP] → SETTLING_ANSWER");
        }
        break;
    }

    case STATE_SETTLING_ANSWER: {
        if (settle_complete) {
            tone_player_chime();
            lvgl_lock();
            magic8ball_ui_set_answer(pending_answer.c_str());
            lvgl_unlock();
            /* Play TTS on background task so main loop never blocks */
            if (tts_mode_str != "off" && pending_answer.length() > 0) {
                tts_task_buf = strdup(pending_answer.c_str());
                if (tts_task_buf) {
                    tts_playing = true;
                    xTaskCreatePinnedToCore(tts_bg_task, "tts", 16384,
                                           NULL, 1, NULL, 1);
                }
            }
            pending_answer = "";
            app_state = STATE_IDLE;
            Serial.println("[LOOP] → IDLE (answer shown)");
        }
        break;
    }

    case STATE_IDLE:
    default: {
        /* legacy LLM result check (no-mic path) */
        String answer;
        if (llm_check_result(answer)) {
            Serial.printf("[LOOP] LLM answer: \"%s\"\n", answer.c_str());
            lvgl_lock();
            magic8ball_ui_stop_anim();
            magic8ball_ui_set_answer(answer.c_str());
            lvgl_unlock();
            tone_player_chime();
        }
        break;
    }

    } /* end switch */

    /* PMIC button: single press = show battery, double press = AP setup */
    if (pmic_ok) {
        xSemaphoreTake(i2c_mux, portMAX_DELAY);
        pmic.getIrqStatus();
        bool short_press = pmic.isPekeyShortPressIrq();
        pmic.clearIrqStatus();
        xSemaphoreGive(i2c_mux);
        if (short_press) {
            unsigned long now = millis();
            gfx->setBrightness(200);
            if (now - last_pkey_ms < DOUBLE_PRESS_WINDOW_MS) {
                pkey_press_count++;
                pkey_single_pending = false;   /* cancel single-press action */
            } else {
                pkey_press_count = 1;
                pkey_single_pending = true;
            }
            last_pkey_ms = now;
            if (pkey_press_count >= 2) {
                Serial.println("[LOOP] Power button double-press!");
                pkey_press_count = 0;
                on_longpress();
            }
        }

        /* Single-press confirmed (double-press window expired) */
        if (pkey_single_pending &&
            millis() - last_pkey_ms >= DOUBLE_PRESS_WINDOW_MS) {
            pkey_single_pending = false;
            if (app_state == STATE_IDLE && !tts_playing) {
                xSemaphoreTake(i2c_mux, portMAX_DELAY);
                int pct = pmic.getBatteryPercent();
                float volts = pmic.getBattVoltage() / 1000.0f;
                bool charging = pmic.isCharging();
                xSemaphoreGive(i2c_mux);
                char buf[32];
                snprintf(buf, sizeof(buf), "Battery\n%d%%\n%.2fV%s",
                         pct, volts, charging ? "\n(charging)" : "");
                lvgl_lock();
                magic8ball_ui_set_answer(buf);
                lvgl_unlock();
                showing_battery = true;
                batt_display_ms = millis();
                Serial.printf("[LOOP] Battery: %d%% %.2fV\n", pct, volts);
            }
        }

        /* Revert battery display back to idle prompt after 2 seconds */
        if (showing_battery && millis() - batt_display_ms > 2000) {
            showing_battery = false;
            lvgl_lock();
            magic8ball_ui_set_answer("TAP TO\nASK");
            lvgl_unlock();
        }
    }

    /* config portal */
    config_portal_loop();

    delay(2);
}
