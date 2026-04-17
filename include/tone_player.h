/*
 *  tone_player.h – I2S feedback sounds via ES8311 DAC
 */
#pragma once

void tone_player_init(void);
void tone_player_beep_listen(void);   /* rising chirp: recording start   */
void tone_player_beep_done(void);     /* falling chirp: recording stop   */
void tone_player_chime(void);         /* mystical chime: answer revealed */
