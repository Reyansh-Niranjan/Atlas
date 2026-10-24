/**
 * @file audio_diag.h
 * @brief ATLAS Hardware Diagnostic - I2S Audio TX (MAX98357A) & RX (INMP441)
 *
 * Pinout:
 *   MAX98357A (TX, I2S_NUM_0):
 *     DIN: GPIO 21, BCLK: GPIO 17, WS: GPIO 18
 *     SD_MODE: GPIO 47 (Active HIGH output)
 *     GAIN: GPIO 2 (Output LOW for 12dB boost)
 *   INMP441 (RX, I2S_NUM_1):
 *     SD: GPIO 16, SCK: GPIO 14, WS: GPIO 15
 *     L/R: Tied to GND (Left Channel)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AUDIO_SAMPLE_RATE    16000
#define AUDIO_SAMPLES_PER_BLOCK 256

typedef enum {
    AUDIO_MODE_SILENCE = 0,
    AUDIO_MODE_TONE_440,
    AUDIO_MODE_TONE_880,
    AUDIO_MODE_LOOPBACK,
} audio_mode_t;

typedef struct {
    uint32_t rms;
    int32_t  peak;
    uint32_t peak_pct; // 0..100
} mic_metrics_t;

/**
 * @brief Initialize both I2S TX (MAX98357A) and I2S RX (INMP441) and control GPIOs.
 */
esp_err_t audio_diag_init(void);

/**
 * @brief Set the active audio output mode (silence, sine tone, or mic loopback).
 */
void audio_diag_set_mode(audio_mode_t mode);

/**
 * @brief Get current audio mode.
 */
audio_mode_t audio_diag_get_mode(void);

/**
 * @brief Get latest live microphone RMS and Peak metrics.
 */
mic_metrics_t audio_diag_get_metrics(void);

/**
 * @brief FreeRTOS task running on Core 0 that streams mic audio, computes RMS,
 *        and pushes tone or loopback to the speaker.
 */
void audio_diag_task(void *pvParam);

#ifdef __cplusplus
}
#endif
