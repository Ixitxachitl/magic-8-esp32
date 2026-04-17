/*
 *  mic_recorder.h – ES8311 microphone recording + voice activity detection
 */
#pragma once

#ifdef __cplusplus
class I2SClass;
I2SClass *mic_recorder_get_i2s(void);
extern "C" {
#endif

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

bool         mic_recorder_init(void);
bool         mic_recorder_start(void);
void         mic_recorder_stop(void);
bool         mic_recorder_is_recording(void);
bool         mic_recorder_speech_ended(void);
int          mic_recorder_get_audio_level(void);   /* 0-100 */
const int16_t *mic_recorder_get_audio_buffer(void);
size_t       mic_recorder_get_sample_count(void);  /* mono samples */
void         mic_recorder_suspend_i2s(void);       /* stop I2S for speaker use */
void         mic_recorder_resume_i2s(void);        /* restart I2S + ES8311 mic */

#ifdef __cplusplus
}
#endif
