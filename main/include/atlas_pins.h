/**
 * @file atlas_pins.h
 * @brief Canonical Hardware Pin & Channel Definitions for the Atlas Handheld (ESP32-S3)
 */
#pragma once

#include "driver/gpio.h"
#include "esp_adc/adc_oneshot.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ── ILI9341 SPI Display (SPI2_HOST / FSPI) ────────────────────────────── */
#define ATLAS_PIN_LCD_MOSI       GPIO_NUM_11
#define ATLAS_PIN_LCD_SCK        GPIO_NUM_12
#define ATLAS_PIN_LCD_MISO       GPIO_NUM_13
#define ATLAS_PIN_LCD_CS         GPIO_NUM_5
#define ATLAS_PIN_LCD_DC         GPIO_NUM_7
#define ATLAS_PIN_LCD_RST        GPIO_NUM_6
#define ATLAS_PIN_LCD_BCKL       ((gpio_num_t)-1) /* Tied to 5V power rail on hardware */

/* ── MicroSD Card SPI Storage (Shared SPI2_HOST with Display) ────────────── */
#define ATLAS_PIN_SD_CS          GPIO_NUM_10
#define ATLAS_SD_MOUNT_POINT     "/sdcard"

/* ── Analog 2-Axis Joystick (ADC1) ───────────────────────────────────────── */
#define ATLAS_ADC_UNIT_JOY       ADC_UNIT_1
#define ATLAS_ADC_CH_JOY_X       ADC_CHANNEL_3    /* GPIO 4 (Swapped: now X-axis) */
#define ATLAS_ADC_CH_JOY_Y       ADC_CHANNEL_0    /* GPIO 1 (Swapped: now Y-axis) */
#define ATLAS_PIN_JOY_X          GPIO_NUM_4
#define ATLAS_PIN_JOY_Y          GPIO_NUM_1
#define ATLAS_PIN_JOY_CLICK      ((gpio_num_t)-1) /* Not wired to switch in hardware */

/* ── Mechanical Switches (Outemu Tactile MK Buttons, Active-LOW) ────────── */
#define ATLAS_PIN_BTN_A          GPIO_NUM_42      /* SW1 */
#define ATLAS_PIN_BTN_B          GPIO_NUM_41      /* SW2 */
#define ATLAS_PIN_BTN_C          GPIO_NUM_40      /* SW3 */
#define ATLAS_PIN_BTN_D          GPIO_NUM_39      /* SW4 */
#define ATLAS_PIN_BTN_MENU       ATLAS_PIN_BTN_C  /* Menu alias for SW3/BTN_C */

/* ── Audio Speaker Amplifier (MAX98357A - I2S_NUM_0 TX) ─────────────────── */
#define ATLAS_PIN_I2S_SPK_DIN    GPIO_NUM_21      /* I2S Serial Data */
#define ATLAS_PIN_I2S_SPK_BCLK   GPIO_NUM_17      /* I2S Bit Clock */
#define ATLAS_PIN_I2S_SPK_WS     GPIO_NUM_18      /* I2S Word Select (LRCLK) */
#define ATLAS_PIN_SPK_SD_MODE    GPIO_NUM_47      /* Amp Enable: HIGH=active, LOW=mute/shutdown */

/* ── Audio Digital Microphone (INMP441 - I2S_NUM_1 RX) ──────────────────── */
#define ATLAS_PIN_I2S_MIC_SD     GPIO_NUM_16      /* I2S Serial Data Out from mic */
#define ATLAS_PIN_I2S_MIC_SCK    GPIO_NUM_14      /* I2S Serial Clock */
#define ATLAS_PIN_I2S_MIC_WS     GPIO_NUM_15      /* I2S Word Select (LRCLK) */

#ifdef __cplusplus
}
#endif
