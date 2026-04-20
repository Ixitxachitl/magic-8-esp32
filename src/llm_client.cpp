/*
 *  llm_client.cpp – OpenAI-compatible LLM API client
 *
 *  Sends a chat-completion request on a background FreeRTOS task so
 *  the LVGL UI keeps rendering while we wait for the network reply.
 */

#include "llm_client.h"
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>

/* ── config ────────────────────────────────────────────────────── */
static String cfg_url;
static String cfg_key;
static String cfg_model;

/* ── inter-task comms ──────────────────────────────────────────── */
static volatile bool     req_pending  = false;
static volatile bool     res_ready    = false;
static String            res_text;
static SemaphoreHandle_t res_mutex    = NULL;
static String            pending_question;

static const char *DEFAULT_SYS_PROMPT =
    "You are a mystical Magic 8 Ball. Give a brief, mysterious answer "
    "to whatever the user is wondering about. Keep your response to "
    "one short sentence (under 10 words). Be creative and mystical. "
    "Do not use quotes. Do not explain yourself. Just give the answer.";

static String custom_sys_prompt;

/* ── background task ───────────────────────────────────────────── */
static void llm_task(void *param)
{
    (void)param;
    String result;
    const int MAX_RETRIES = 2;

    Serial.println("[LLM] Task started");

    for (int attempt = 0; attempt <= MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            Serial.printf("[LLM] Retry %d/%d\n", attempt, MAX_RETRIES);
            delay(500);
        }
        result = "";

        WiFiClientSecure *client = new WiFiClientSecure();
        if (!client) {
            result = "Out of memory";
            Serial.println("[LLM] Failed to allocate WiFiClientSecure");
            break;
        }
        client->setInsecure();

        HTTPClient http;
        String url = cfg_url;
        if (!url.endsWith("/")) url += "/";
        url += "chat/completions";
        if (attempt == 0) {
            Serial.printf("[LLM] POST %s\n", url.c_str());
            Serial.printf("[LLM] Model: %s\n", cfg_model.c_str());
        }

        if (!http.begin(*client, url)) {
            result = "Connection\nfailed";
            Serial.println("[LLM] http.begin() failed");
            delete client;
            continue;  /* retry */
        }

        http.addHeader("Content-Type", "application/json");
        if (cfg_key.length() > 0) {
            http.addHeader("Authorization", "Bearer " + cfg_key);
            if (attempt == 0)
                Serial.printf("[LLM] Auth header set (key len=%d)\n", cfg_key.length());
        } else {
            Serial.println("[LLM] WARNING: No API key set");
        }
        http.setTimeout(15000);

        /* build JSON payload */
        JsonDocument doc;
        doc["model"]      = cfg_model;
        doc["max_completion_tokens"] = 1024;
        doc["temperature"] = 1.2;

        JsonArray msgs = doc["messages"].to<JsonArray>();
        JsonObject sm  = msgs.add<JsonObject>();
        sm["role"]    = "system";
        sm["content"] = custom_sys_prompt.length() > 0
                        ? custom_sys_prompt.c_str()
                        : DEFAULT_SYS_PROMPT;
        JsonObject um  = msgs.add<JsonObject>();
        um["role"]    = "user";
        um["content"] = pending_question.length() > 0
                        ? pending_question.c_str()
                        : "Give me a Magic 8 Ball answer.";

        String payload;
        serializeJson(doc, payload);
        if (attempt == 0)
            Serial.printf("[LLM] Payload (%d bytes): %s\n", payload.length(), payload.c_str());

        unsigned long t0 = millis();
        int code = http.POST(payload);
        unsigned long elapsed = millis() - t0;
        Serial.printf("[LLM] HTTP response: %d (%lu ms)\n", code, elapsed);

        if (code == 200) {
            String body = http.getString();
            Serial.printf("[LLM] Body (%d bytes): %s\n", body.length(), body.c_str());
            JsonDocument resp;
            DeserializationError err = deserializeJson(resp, body);
            if (!err) {
                result = resp["choices"][0]["message"]["content"].as<String>();
                result.trim();
                result.replace("\"", "");
                result.replace("*", "");
                /* Replace Unicode chars not in LVGL Montserrat (Basic Latin only) */
                result.replace("\xe2\x80\x94", "-");   /* em dash — */
                result.replace("\xe2\x80\x93", "-");   /* en dash – */
                result.replace("\xe2\x80\x98", "'");   /* left single quote ' */
                result.replace("\xe2\x80\x99", "'");   /* right single quote ' */
                result.replace("\xe2\x80\x9c", "\"");  /* left double quote " */
                result.replace("\xe2\x80\x9d", "\"");  /* right double quote " */
                result.replace("\xe2\x80\xa6", "...");  /* ellipsis … */
                if (result.length() > 200)
                    result = result.substring(0, 197) + "...";
                Serial.printf("[LLM] Answer: \"%s\"\n", result.c_str());
            } else {
                result = "The spirits\nare confused";
                Serial.printf("[LLM] JSON parse error: %s\n", err.c_str());
            }
            http.end();
            delete client;
            break;  /* success — stop retrying */
        }

        String errBody = http.getString();
        Serial.printf("[LLM] HTTP error %d: %s\n", code, errBody.c_str());
        http.end();
        delete client;

        /* Only retry on timeouts, server errors, and rate limits */
        if (code == 429) {
            Serial.println("[LLM] Rate limited – will retry");
            result = "Cannot reach\nthe beyond";
            delay(3000);  /* brief backoff before retry */
            /* loop continues to retry */
        } else if (code >= 400 && code < 500) {
            result = "Cannot reach\nthe beyond";
            break;  /* don't retry other client errors */
        } else {
            result = "Cannot reach\nthe beyond";
            /* loop continues to retry */
        }
    }

    xSemaphoreTake(res_mutex, portMAX_DELAY);
    res_text    = result;
    res_ready   = true;
    req_pending = false;
    xSemaphoreGive(res_mutex);

    vTaskDelete(NULL);
}

/* ── public API ────────────────────────────────────────────────── */

void llm_set_system_prompt(const String &prompt)
{
    custom_sys_prompt = prompt;
    Serial.printf("[LLM] System prompt set (%d chars)\n", prompt.length());
}

void llm_init(const String &url, const String &key, const String &model)
{
    cfg_url   = url;
    cfg_key   = key;
    cfg_model = model;
    if (!res_mutex) res_mutex = xSemaphoreCreateMutex();
    Serial.println("[LLM] Initialized");
    Serial.printf("[LLM]   URL:   %s\n", url.c_str());
    Serial.printf("[LLM]   Model: %s\n", model.c_str());
    Serial.printf("[LLM]   Key:   %d chars\n", key.length());
}

void llm_start_request(void)
{
    if (req_pending) {
        Serial.println("[LLM] Request already pending, skipping");
        return;
    }
    Serial.println("[LLM] Starting new request...");
    pending_question = "";
    req_pending = true;
    res_ready   = false;
    xTaskCreatePinnedToCore(llm_task, "llm", 16384, NULL, 1, NULL, 0);
}

void llm_start_request_with_question(const String &question)
{
    if (req_pending) {
        Serial.println("[LLM] Request already pending, skipping");
        return;
    }
    Serial.printf("[LLM] Starting request with question: \"%s\"\n", question.c_str());
    pending_question = question;
    req_pending = true;
    res_ready   = false;
    xTaskCreatePinnedToCore(llm_task, "llm", 16384, NULL, 1, NULL, 0);
}

bool llm_is_busy(void) { return req_pending; }

bool llm_check_result(String &out)
{
    if (!res_ready) return false;
    xSemaphoreTake(res_mutex, portMAX_DELAY);
    out         = res_text;
    res_ready   = false;
    xSemaphoreGive(res_mutex);
    return true;
}
