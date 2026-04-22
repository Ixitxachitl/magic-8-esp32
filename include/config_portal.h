#pragma once
#include <Arduino.h>

void config_portal_begin(void);
void config_portal_loop(void);
void config_portal_start_ap(void);
bool config_portal_is_configured(void);
bool config_portal_is_ap_mode(void);
String config_portal_get_api_url(void);
String config_portal_get_api_key(void);
String config_portal_get_model(void);
String config_portal_get_system_prompt(void);
String config_portal_get_tts_mode(void);
String config_portal_get_tts_voice(void);
String config_portal_get_stt_provider(void);
String config_portal_get_stt_url(void);
String config_portal_get_stt_key(void);
String config_portal_get_tts_key(void);
String config_portal_get_tts_url(void);

/* Called whenever provider or key settings are saved from the portal */
void config_portal_set_settings_changed_cb(void (*cb)(void));
