/**
 * @file atlas_init.h
 * @brief Atlas OS Boot Engine & Hardware Self-Check Diagnostics
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     psram_ok;
    size_t   psram_size;
    bool     display_ok;
    bool     input_ok;
    bool     audio_ok;
    bool     sdcard_ok;
    uint64_t sdcard_size_bytes;
    bool     joystick_cal_ok;
    int16_t  joy_center_x;
    int16_t  joy_center_y;
    bool     mic_data_ok;
    bool     recovery_triggered;
} atlas_system_diag_t;

/**
 * @brief Execute the Atlas OS bootstrap sequence:
 * 1. PSRAM verification
 * 2. Display early initialization & solid black clear (0x0000)
 * 3. Storage mount (/sdcard over SPI2)
 * 4. Splash screen rendering (/sdcard/assets/R2_Reyansh-LOGO.jpg or centered text banner)
 * 5. Input (ADC1 + MK switches) & Audio (I2S0 + I2S1) init
 * 6. Component diagnostics (PSRAM, SD mount, Joystick center, Mic RX clock/data; outputs bypassed)
 * 7. Recovery check: If Button B is held, jump into Recovery Safe Mode.
 *
 * @param out_diag Optional pointer to receive diagnostics report.
 * @return ESP_OK on successful bootstrap.
 */
esp_err_t atlas_system_init(atlas_system_diag_t *out_diag);

#ifdef __cplusplus
}
#endif
