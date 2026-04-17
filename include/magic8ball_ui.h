#pragma once
#include <lvgl.h>

#ifdef __cplusplus
extern "C" {
#endif

void magic8ball_ui_init(lv_obj_t *parent, int screen_size);
void magic8ball_ui_set_answer(const char *text);
void magic8ball_ui_set_thinking(bool thinking);
void magic8ball_ui_set_tap_cb(void (*cb)(void));
void magic8ball_ui_set_longpress_cb(void (*cb)(void));
void magic8ball_ui_set_listening(bool listening);
void magic8ball_ui_set_transcript(const char *text);
void magic8ball_ui_set_audio_level(int level);   /* 0-100 */
void magic8ball_ui_start_anim(void);             /* spinning + bubbles */
void magic8ball_ui_stop_anim(void);              /* stop, reset angle  */
void magic8ball_ui_settle_then(void (*done_cb)(void));  /* decelerate then callback */

#ifdef __cplusplus
}
#endif
