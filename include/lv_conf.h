/**
 * lv_conf.h – LVGL 8.4 configuration for Magic 8 Ball
 * Target: ESP32-S3-Touch-AMOLED-1.75C (466×466 CO5300)
 */

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/* ── Colour depth ─────────────────────────────────────────── */
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1

/* ── Memory ───────────────────────────────────────────────── */
#ifdef ARDUINO
  #define LV_MEM_CUSTOM            1
  #define LV_MEM_CUSTOM_INCLUDE    "esp_heap_caps.h"
  #define LV_MEM_CUSTOM_ALLOC(size)        heap_caps_malloc(size, MALLOC_CAP_SPIRAM)
  #define LV_MEM_CUSTOM_FREE               heap_caps_free
  #define LV_MEM_CUSTOM_REALLOC(ptr, size) heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM)
#else
  #define LV_MEM_CUSTOM      0
  #define LV_MEM_SIZE        (512U * 1024U)
#endif

/* ── Display ──────────────────────────────────────────────── */
#define LV_HOR_RES_MAX     466
#define LV_VER_RES_MAX     466
#define LV_DPI_DEF         200

/* ── Tick ─────────────────────────────────────────────────── */
#define LV_DISP_DEF_REFR_PERIOD 16
#define LV_TICK_CUSTOM      1
#ifdef ARDUINO
  #define LV_TICK_CUSTOM_INCLUDE          "Arduino.h"
  #define LV_TICK_CUSTOM_SYS_TIME_EXPR    ((uint32_t)millis())
#else
  uint32_t lv_sdl_tick_get(void);
  #define LV_TICK_CUSTOM_INCLUDE          <stdint.h>
  #define LV_TICK_CUSTOM_SYS_TIME_EXPR    (lv_sdl_tick_get())
#endif

/* ── Logging ──────────────────────────────────────────────── */
#define LV_USE_LOG         0

/* ── GPU / draw ───────────────────────────────────────────── */
#define LV_USE_GPU_STM32_DMA2D    0
#define LV_USE_GPU_NXP_PXP        0
#define LV_USE_GPU_NXP_VG_LITE    0
#define LV_USE_GPU_SDL            0

/* ── Fonts ────────────────────────────────────────────────── */
#define LV_FONT_MONTSERRAT_14     1
#define LV_FONT_MONTSERRAT_16     1
#define LV_FONT_MONTSERRAT_20     1
#define LV_FONT_MONTSERRAT_24     1
#define LV_FONT_MONTSERRAT_28     1
#define LV_FONT_MONTSERRAT_36     1
#define LV_FONT_DEFAULT           &lv_font_montserrat_20

/* ── Widgets ──────────────────────────────────────────────── */
#define LV_USE_ARC        0
#define LV_USE_BAR        0
#define LV_USE_BTN        1
#define LV_USE_BTNMATRIX  0
#define LV_USE_CANVAS     0
#define LV_USE_CHECKBOX   0
#define LV_USE_DROPDOWN   0
#define LV_USE_IMG        1
#define LV_USE_LABEL      1
#define LV_USE_LINE       1
#define LV_USE_ROLLER     0
#define LV_USE_SLIDER     0
#define LV_USE_SWITCH     0
#define LV_USE_TEXTAREA   0
#define LV_USE_TABLE      0
#define LV_USE_METER      0

/* ── Extra widgets ────────────────────────────────────────── */
#define LV_USE_ANIMIMG    0
#define LV_USE_CALENDAR   0
#define LV_USE_CHART      0
#define LV_USE_COLORWHEEL 0
#define LV_USE_IMGBTN     0
#define LV_USE_KEYBOARD   0
#define LV_USE_LED        0
#define LV_USE_LIST       0
#define LV_USE_MENU       0
#define LV_USE_MSGBOX     0
#define LV_USE_SPAN       0
#define LV_USE_SPINBOX    0
#define LV_USE_SPINNER    0
#define LV_USE_TABVIEW    0
#define LV_USE_TILEVIEW   0
#define LV_USE_WIN        0

/* ── Themes ───────────────────────────────────────────────── */
#define LV_USE_THEME_DEFAULT   1
#define LV_THEME_DEFAULT_DARK  1
#define LV_USE_THEME_BASIC     1
#define LV_USE_THEME_MONO      0

/* ── Demos (disabled) ─────────────────────────────────────── */
#define LV_USE_DEMO_WIDGETS            0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK          0
#define LV_USE_DEMO_STRESS             0
#define LV_USE_DEMO_MUSIC              0

/* ── File system (disabled) ───────────────────────────────── */
#define LV_USE_FS_STDIO    0
#define LV_USE_FS_POSIX    0
#define LV_USE_FS_WIN32    0
#define LV_USE_FS_FATFS    0

/* ── Others ───────────────────────────────────────────────── */
#define LV_USE_SNAPSHOT    0
#define LV_BUILD_EXAMPLES  0

#endif /* LV_CONF_H */
