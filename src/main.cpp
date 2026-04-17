/*
 *  main.cpp – Magic 8 Ball
 *  Target: Waveshare ESP32-S3-Touch-AMOLED-1.75C
 *          CO5300 466×466 QSPI AMOLED
 *
 *  Tap the ball or shake the device → asks a free LLM for a mystical
 *  one-line answer displayed inside a classic blue-triangle window.
 */

#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <lvgl.h>
#include <math.h>
#include "Arduino_GFX_Library.h"
#include <SensorQMI8658.hpp>
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
static const uint32_t STAGING_LINES  = 40;
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

static void my_touchpad_read(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    int16_t x[1], y[1];
    if (touch_ok && touch.getPoint(x, y, 1)) {
        data->point.x = (LCD_WIDTH  - 1) - x[0];
        data->point.y = (LCD_HEIGHT - 1) - y[0];
        data->state   = LV_INDEV_STATE_PR;
    } else {
        data->state = LV_INDEV_STATE_REL;
    }
}

/* ── IMU (shake detection) ─────────────────────────────────────── */
SensorQMI8658 imu;
static bool imu_ok = false;
#define SHAKE_COOLDOWN_MS 2000
#define SHAKE_JOLT_THRESH 0.15f   /* 0.15 g above baseline */
static unsigned long last_shake_ms  = 0;
static float         acc_baseline   = 0;

/* ── PMIC ──────────────────────────────────────────────────────── */
XPowersAXP2101 pmic;
static bool pmic_ok = false;

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
static bool           final_stt_sent     = false;
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
        if (tts_mode_str == "groq") {
            tts_groq_say(tts_task_buf);
        } else if (tts_mode_str == "sam") {
            tts_say(tts_task_buf);
        }
        free(tts_task_buf);
        tts_task_buf = NULL;
    }
    tts_playing = false;
    vTaskDelete(NULL);
}

static void on_settle_done(void) { settle_complete = true; }

static void on_ask(void)
{
    if (app_state != STATE_IDLE) {
        Serial.println("[MAIN] on_ask: not idle, ignoring");
        return;
    }

    Serial.println("[MAIN] on_ask: Asking the 8 Ball...");
    magic8ball_ui_start_anim();

    /* If mic + LLM available → voice flow */
    if (use_llm && mic_ok) {
        tone_player_beep_listen();
        mic_recorder_start();
        magic8ball_ui_set_listening(true);
        app_state      = STATE_LISTENING;
        last_partial_ms = millis();
        Serial.println("[MAIN] Entered LISTENING state (voice)");
        return;
    }

    /* LLM without mic → direct request */
    if (use_llm) {
        magic8ball_ui_set_thinking(true);
        llm_start_request();
        app_state = STATE_ASKING_LLM;
        Serial.println("[MAIN] Entered ASKING_LLM state (no mic)");
        return;
    }

    /* Offline → classic answer */
    magic8ball_ui_set_thinking(true);
    int idx = random(0, NUM_CLASSIC);
    Serial.printf("[MAIN] Offline, classic answer #%d\n", idx);
    static int chosen;
    chosen = idx;
    lv_timer_create([](lv_timer_t *t) {
        magic8ball_ui_stop_anim();
        magic8ball_ui_set_answer(CLASSIC[*((int *)t->user_data)]);
        lv_timer_del(t);
    }, 1200, &chosen);
    /* stay IDLE – timer callback shows answer */
}

static void on_longpress(void)
{
    Serial.println("[MAIN] Long-press detected – entering AP setup");
    magic8ball_ui_set_answer("Entering\nsetup...");
    lv_timer_handler();
    delay(800);
    config_portal_start_ap();
    magic8ball_ui_set_answer("Connect WiFi:\nMagic8Ball\n-Setup\n192.168.4.1");
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

    /* ── IMU (accel only, for shake) ─────────────────────────── */
    if (imu.begin(Wire, QMI8658_L_SLAVE_ADDRESS)) {
        imu_ok = true;
        imu.configAccelerometer(SensorQMI8658::ACC_RANGE_4G,
                                SensorQMI8658::ACC_ODR_62_5Hz,
                                SensorQMI8658::LPF_MODE_0);
        imu.enableAccelerometer();
        Serial.println("[INIT] IMU QMI8658 OK (accel 4G, 62.5Hz)");
    } else {
        Serial.println("[INIT] IMU QMI8658 NOT FOUND – shake disabled");
    }

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
    magic8ball_ui_set_longpress_cb(on_longpress);

    /* first render so the user sees something during WiFi connect */
    magic8ball_ui_set_answer("Connecting\nto WiFi...");
    lv_timer_handler();

    /* ── WiFi + config portal ────────────────────────────────── */    Serial.println("[INIT] Starting WiFi / config portal...");    config_portal_begin();

    if (config_portal_is_configured()) {
        String key = config_portal_get_api_key();
        if (key.length() > 0) {
            llm_init(config_portal_get_api_url(),
                     key,
                     config_portal_get_model());
            llm_set_system_prompt(config_portal_get_system_prompt());
            stt_init(config_portal_get_api_url(), key);
            use_llm = true;
            Serial.println("[INIT] LLM + STT mode ENABLED (online)");
        } else {
            Serial.println("[INIT] No API key – classic offline mode");
        }

        /* ── Microphone (ES8311) ─────────────────────────────── */
        mic_ok = mic_recorder_init();
        Serial.printf("[INIT] Microphone %s\n", mic_ok ? "OK" : "FAIL (voice disabled)");

        /* ── Tone player (needs mic I2S instance) ────────────── */
        if (mic_ok) {
            tone_player_init();
            Serial.println("[INIT] Tone player OK");

            /* ── TTS engine selection ────────────────────────── */
            tts_mode_str = config_portal_get_tts_mode();
            if (tts_mode_str == "groq" && use_llm) {
                tts_groq_init(config_portal_get_api_url(),
                              config_portal_get_api_key());
                tts_groq_set_voice(config_portal_get_tts_voice());
                Serial.println("[INIT] TTS: Groq Orpheus");
            } else if (tts_mode_str != "off") {
                tts_mode_str = "sam";
                tts_init();
                Serial.println("[INIT] TTS: SAM (local)");
            } else {
                Serial.println("[INIT] TTS: Off");
            }
        }

        magic8ball_ui_set_answer("SHAKE\nOR TAP");
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
    /* service LVGL */
    lv_timer_handler();

    /* ── state machine ─────────────────────────────────────────── */
    switch (app_state) {

    case STATE_LISTENING: {
        /* audio-level → triangle glow */
        magic8ball_ui_set_audio_level(mic_recorder_get_audio_level());

        /* pick up any partial STT result */
        String partial;
        if (stt_check_result(partial) && partial.length() > 0) {
            Serial.printf("[LOOP] Partial transcript: \"%s\"\n", partial.c_str());
            magic8ball_ui_set_transcript(partial.c_str());
        }

        /* fire partial STT every 3 s while enough audio exists */
        if (millis() - last_partial_ms > 3000 &&
            !stt_is_busy() &&
            mic_recorder_get_sample_count() > 16000) {       /* >1 s */
            Serial.println("[LOOP] Sending partial audio to STT...");
            stt_start_transcribe(mic_recorder_get_audio_buffer(),
                                 mic_recorder_get_sample_count());
            last_partial_ms = millis();
        }

        /* speech ended → transition to final transcription */
        if (mic_recorder_speech_ended()) {
            Serial.println("[LOOP] Speech ended → TRANSCRIBING");
            tone_player_beep_done();
            app_state       = STATE_TRANSCRIBING;
            final_stt_sent  = false;
            transcript_shown_ms = 0;
        }

        /* timeout: back out if no speech for 8 seconds */
        if (millis() - last_partial_ms > 8000 &&
            mic_recorder_get_sample_count() < 16000) {
            Serial.println("[LOOP] Listening timeout – no speech detected");
            mic_recorder_stop();
            magic8ball_ui_stop_anim();
            magic8ball_ui_set_answer("SHAKE\nOR TAP");
            app_state = STATE_IDLE;
        }
        break;
    }

    case STATE_TRANSCRIBING: {
        /* drain any in-flight partial result first */
        String discard;
        if (!final_stt_sent) {
            stt_check_result(discard);
            if (!stt_is_busy()) {
                stt_start_transcribe(mic_recorder_get_audio_buffer(),
                                     mic_recorder_get_sample_count());
                final_stt_sent = true;
                Serial.println("[LOOP] Final STT request sent");
            }
        } else {
            String text;
            if (stt_check_result(text)) {
                if (text.length() > 0) {
                    Serial.printf("[LOOP] Final transcript: \"%s\"\n", text.c_str());
                    final_transcript = text;
                    /* settle spin before showing transcript */
                    settle_complete = false;
                    magic8ball_ui_settle_then(on_settle_done);
                    app_state = STATE_SETTLING_TRANSCRIPT;
                    Serial.println("[LOOP] → SETTLING_TRANSCRIPT");
                } else {
                    Serial.println("[LOOP] Empty transcript – back to idle");
                    magic8ball_ui_stop_anim();
                    magic8ball_ui_set_answer("I didn't\nhear anything");
                    app_state = STATE_IDLE;
                }
            }
        }
        break;
    }

    case STATE_SETTLING_TRANSCRIPT: {
        if (settle_complete) {
            magic8ball_ui_set_transcript(final_transcript.c_str());
            transcript_shown_ms = millis();
            app_state = STATE_SHOWING_TRANSCRIPT;
            Serial.println("[LOOP] → SHOWING_TRANSCRIPT");
        }
        break;
    }

    case STATE_SHOWING_TRANSCRIPT: {
        /* show transcript for 1.5 s, then resume spin and ask LLM */
        if (millis() - transcript_shown_ms > 1500) {
            magic8ball_ui_start_anim();
            llm_start_request_with_question(final_transcript);
            magic8ball_ui_set_thinking(true);
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
            magic8ball_ui_settle_then(on_settle_done);
            app_state = STATE_SETTLING_ANSWER;
            Serial.println("[LOOP] → SETTLING_ANSWER");
        }
        break;
    }

    case STATE_SETTLING_ANSWER: {
        if (settle_complete) {
            tone_player_chime();
            magic8ball_ui_set_answer(pending_answer.c_str());
            /* Flush LVGL so the answer text is visible */
            lv_timer_handler();
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
            magic8ball_ui_stop_anim();
            tone_player_chime();
            magic8ball_ui_set_answer(answer.c_str());
        }
        break;
    }

    } /* end switch */

    /* ── shake detection (any state == IDLE) ────────────────────── */
    if (imu_ok) {
        float ax, ay, az;
        if (imu.getAccelerometer(ax, ay, az)) {
            float mag = sqrtf(ax * ax + ay * ay + az * az);

            /* Adaptive baseline: faster EMA so it settles quickly */
            if (acc_baseline < 0.01f) acc_baseline = mag;
            else acc_baseline = acc_baseline * 0.9f + mag * 0.1f;

            float jolt = fabsf(mag - acc_baseline);

            /* Trigger on fixed threshold (0.15 g above baseline) */
            if (jolt > SHAKE_JOLT_THRESH) {
                unsigned long now = millis();
                if (now - last_shake_ms > SHAKE_COOLDOWN_MS &&
                    app_state == STATE_IDLE) {
                    last_shake_ms = now;
                    Serial.printf("[LOOP] SHAKE! mag=%.2f jolt=%.2f\n", mag, jolt);
                    on_ask();
                }
            }
        }
    }

    /* PMIC button: short press wakes screen */
    if (pmic_ok) {
        pmic.getIrqStatus();
        if (pmic.isPekeyShortPressIrq()) {
            Serial.println("[LOOP] Power button pressed – screen ON");
            gfx->setBrightness(200);
        }
        pmic.clearIrqStatus();
    }

    /* config portal */
    config_portal_loop();

    delay(2);
}
