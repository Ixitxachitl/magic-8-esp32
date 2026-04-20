/*
 *  tts_groq.h – Text-to-speech via Groq Orpheus API
 *
 *  Calls Groq's /audio/speech endpoint, downloads WAV, plays through I2S.
 *  Requires network. Falls back silently on failure.
 */
#pragma once

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

void tts_groq_init(const String &api_url, const String &api_key);
void tts_groq_set_voice(const String &voice);
void tts_groq_set_model(const String &model);

/*  Speak a string.  Blocks until finished.  Safe to call from loop().
 *  Input limited to 200 chars by Groq API. */
void tts_groq_say(const char *text);

#ifdef __cplusplus
}
#endif
