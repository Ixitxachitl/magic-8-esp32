/*
 *  tts_elevenlabs.h – Text-to-speech via ElevenLabs API
 *
 *  POST /v1/text-to-speech/{voice_id}?output_format=pcm_16000
 *  Returns raw 16-bit mono PCM at 16 kHz. Plays through shared I2S bus.
 */
#pragma once

#include <Arduino.h>

#ifdef __cplusplus
extern "C" {
#endif

void tts_elevenlabs_init(const String &api_key);
void tts_elevenlabs_set_voice(const String &voice_id);
void tts_elevenlabs_say(const char *text);

#ifdef __cplusplus
}
#endif
