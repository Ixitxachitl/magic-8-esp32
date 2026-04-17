/*
 *  tts_sam.h – Text-to-speech using SAM (Software Automatic Mouth)
 *
 *  Generates speech on the ESP32 and plays it through the ES8311 DAC.
 *  No network required – everything runs locally.
 */
#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void tts_init(void);

/*  Speak a string.  Blocks until finished.  Safe to call from loop().
 *  Text is limited to ~250 chars by SAM.  */
void tts_say(const char *text);

#ifdef __cplusplus
}
#endif
