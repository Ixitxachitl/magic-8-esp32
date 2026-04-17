#pragma once
#include <Arduino.h>

void llm_init(const String &url, const String &key, const String &model);
void llm_set_system_prompt(const String &prompt);
void llm_start_request(void);
void llm_start_request_with_question(const String &question);
bool llm_is_busy(void);
bool llm_check_result(String &result);
