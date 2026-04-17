/*
 * es7210.cpp – ES7210 quad-channel ADC driver
 * From Waveshare ESP32-S3-Touch-AMOLED-1.75C examples (Espressif MIT License)
 */
#include <Arduino.h>
#include "es7210.h"

static TwoWire *_tw = NULL;
static es7210_gain_value_t _gain_mic1 = GAIN_0DB;
static es7210_gain_value_t _gain_mic2 = GAIN_0DB;
static es7210_gain_value_t _gain_mic3 = GAIN_0DB;
static es7210_gain_value_t _gain_mic4 = GAIN_0DB;

static int es7210_write_reg(uint8_t reg_addr, uint8_t data)
{
    _tw->beginTransmission(ES7210_ADDR);
    _tw->write(reg_addr);
    _tw->write(data);
    return _tw->endTransmission();
}

int es7210_read_reg(uint8_t reg_addr)
{
    _tw->beginTransmission(ES7210_ADDR);
    _tw->write(reg_addr);
    _tw->endTransmission(false);
    _tw->requestFrom((uint8_t)ES7210_ADDR, (uint8_t)1);
    if (_tw->available()) {
        return _tw->read();
    }
    return -1;
}

esp_err_t es7210_adc_init(TwoWire *tw, audio_hal_codec_config_t *codec_cfg)
{
    _tw = tw;
    esp_err_t ret = ESP_OK;

    /*
     * Init sequence matched to Espressif esp-bsp es7210 component driver
     * (es7210_config_codec) for reliable operation.
     */

    /* Software reset → enter config mode (0x32, NOT 0x41 yet) */
    ret |= es7210_write_reg(ES7210_RESET_REG00, 0xFF);
    ret |= es7210_write_reg(ES7210_RESET_REG00, 0x32);

    /* Enable all clocks (default R01=0x20 has a clock gated) */
    ret |= es7210_write_reg(ES7210_CLOCK_OFF_REG01, 0x00);

    /* Initialization timing */
    ret |= es7210_write_reg(ES7210_TIME_CONTROL0_REG09, 0x30);
    ret |= es7210_write_reg(ES7210_TIME_CONTROL1_REG0A, 0x30);

    /* HPF for ADC1-4 (component driver configures these) */
    ret |= es7210_write_reg(0x23, 0x2A);   /* ADC12_HPF1 */
    ret |= es7210_write_reg(0x22, 0x0A);   /* ADC12_HPF2 */
    ret |= es7210_write_reg(0x21, 0x2A);   /* ADC34_HPF1 */
    ret |= es7210_write_reg(0x20, 0x0A);   /* ADC34_HPF2 */

    /* I2S format: I2S standard, 16-bit word, no TDM */
    ret |= es7210_write_reg(ES7210_SDP_INTERFACE1_REG11, 0x60);
    ret |= es7210_write_reg(ES7210_SDP_INTERFACE2_REG12, 0x00);

    /* Analog power (0xC3 — bit 7 required for proper analog operation) */
    ret |= es7210_write_reg(ES7210_ANALOG_REG40, 0xC3);

    /* MIC1-4 bias */
    ret |= es7210_write_reg(ES7210_MIC12_BIAS_REG41, 0x70);
    ret |= es7210_write_reg(ES7210_MIC34_BIAS_REG42, 0x70);

    /* MIC1-4 gain (bit 4 = gain enable, must be OR'd) */
    ret |= es7210_write_reg(ES7210_MIC1_GAIN_REG43, GAIN_30DB | 0x10);
    ret |= es7210_write_reg(ES7210_MIC2_GAIN_REG44, GAIN_30DB | 0x10);
    ret |= es7210_write_reg(ES7210_MIC3_GAIN_REG45, GAIN_0DB | 0x10);
    ret |= es7210_write_reg(ES7210_MIC4_GAIN_REG46, GAIN_0DB | 0x10);

    /* MIC1-4 power on */
    ret |= es7210_write_reg(ES7210_MIC1_POWER_REG47, 0x08);
    ret |= es7210_write_reg(ES7210_MIC2_POWER_REG48, 0x08);
    ret |= es7210_write_reg(ES7210_MIC3_POWER_REG49, 0x08);
    ret |= es7210_write_reg(ES7210_MIC4_POWER_REG4A, 0x08);

    /* Sample rate: 16 kHz with 4.096 MHz MCLK (256×) */
    ret |= es7210_write_reg(ES7210_OSR_REG07, 0x20);
    ret |= es7210_write_reg(ES7210_MAINCLK_REG02, 0xC1);
    ret |= es7210_write_reg(ES7210_LRCK_DIVH_REG04, 0x01);
    ret |= es7210_write_reg(ES7210_LRCK_DIVL_REG05, 0x00);

    /* Power down DLL (component driver does this) */
    ret |= es7210_write_reg(ES7210_POWER_DOWN_REG06, 0x04);

    /* Enable MIC12 & MIC34 power */
    ret |= es7210_write_reg(ES7210_MIC12_POWER_REG4B, 0x0F);
    ret |= es7210_write_reg(ES7210_MIC34_POWER_REG4C, 0x0F);

    /* Enable device (two-step: 0x71 → 0x41) */
    ret |= es7210_write_reg(ES7210_RESET_REG00, 0x71);
    ret |= es7210_write_reg(ES7210_RESET_REG00, 0x41);

    return ret;
}

esp_err_t es7210_adc_deinit(void)
{
    return es7210_write_reg(ES7210_RESET_REG00, 0xFF);
}

esp_err_t es7210_adc_config_i2s(audio_hal_codec_mode_t mode, audio_hal_codec_i2s_iface_t *iface)
{
    (void)mode;
    (void)iface;
    return ESP_OK;
}

esp_err_t es7210_adc_ctrl_state(audio_hal_codec_mode_t mode, audio_hal_ctrl_t ctrl_state)
{
    (void)mode;
    (void)ctrl_state;
    return ESP_OK;
}

esp_err_t es7210_adc_set_gain(es7210_input_mics_t mic_mask, es7210_gain_value_t gain)
{
    esp_err_t ret = ESP_OK;
    if (mic_mask & ES7210_INPUT_MIC1) {
        ret |= es7210_write_reg(ES7210_MIC1_GAIN_REG43, gain | 0x10);
        _gain_mic1 = gain;
    }
    if (mic_mask & ES7210_INPUT_MIC2) {
        ret |= es7210_write_reg(ES7210_MIC2_GAIN_REG44, gain | 0x10);
        _gain_mic2 = gain;
    }
    if (mic_mask & ES7210_INPUT_MIC3) {
        ret |= es7210_write_reg(ES7210_MIC3_GAIN_REG45, gain | 0x10);
        _gain_mic3 = gain;
    }
    if (mic_mask & ES7210_INPUT_MIC4) {
        ret |= es7210_write_reg(ES7210_MIC4_GAIN_REG46, gain | 0x10);
        _gain_mic4 = gain;
    }
    return ret;
}

esp_err_t es7210_adc_set_gain_all(es7210_gain_value_t gain)
{
    return es7210_adc_set_gain(
        (es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2 |
                              ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4),
        gain);
}

esp_err_t es7210_adc_get_gain(es7210_input_mics_t mic_mask, es7210_gain_value_t *gain)
{
    if (!gain) return ESP_FAIL;
    if (mic_mask & ES7210_INPUT_MIC1) *gain = _gain_mic1;
    else if (mic_mask & ES7210_INPUT_MIC2) *gain = _gain_mic2;
    else if (mic_mask & ES7210_INPUT_MIC3) *gain = _gain_mic3;
    else if (mic_mask & ES7210_INPUT_MIC4) *gain = _gain_mic4;
    return ESP_OK;
}

void es7210_read_all(void)
{
    for (int i = 0; i <= 0x4C; i++) {
        int val = es7210_read_reg(i);
        Serial.printf("[ES7210] REG 0x%02X = 0x%02X\n", i, val);
    }
}
