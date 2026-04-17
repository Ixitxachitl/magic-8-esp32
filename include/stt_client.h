/*
 *  stt_client.h – Speech-to-text via Groq Whisper API
 */
#pragma once
#include <Arduino.h>

void stt_init(const String &api_url, const String &api_key);
void stt_start_transcribe(const int16_t *samples, size_t sample_count);
bool stt_is_busy(void);
bool stt_check_result(String &result);
