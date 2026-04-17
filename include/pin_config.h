#pragma once

// ── Power management ────────────────────────────────
#define XPOWERS_CHIP_AXP2101

// ── QSPI LCD (CO5300 AMOLED) ────────────────────────
#define LCD_SDIO0   4
#define LCD_SDIO1   5
#define LCD_SDIO2   6
#define LCD_SDIO3   7
#define LCD_SCLK   38
#define LCD_CS     12
#define LCD_RESET   2

#define LCD_WIDTH  466
#define LCD_HEIGHT 466

// ── I2C bus ─────────────────────────────────────────
#define IIC_SDA    15
#define IIC_SCL    14

// ── Touch (CST9217) ─────────────────────────────────
#define TP_INT     11
#define TP_RST      2

// ── Audio (ES8311 I2S codec + PA) ───────────────────
#define I2S_MCK_IO  16
#define I2S_BCK_IO   9
#define I2S_WS_IO   45
#define I2S_DO_IO    8
#define I2S_DI_IO   10
#define PA          46

// ── Microphone (ES7210 dual digital mic array) ──────
#define PIN_ES7210_MCLK  16
#define PIN_ES7210_BCLK   9
#define PIN_ES7210_LRCK  45
#define PIN_ES7210_DIN   10
