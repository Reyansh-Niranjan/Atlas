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

/**
 * @brief Per-component boot diagnostic snapshot.
 *
 * Fields are populated by atlas_system_init() regardless of success or failure
 * so the caller and the recovery screen can display truthful information.
 */
typedef struct {
    bool     psram_ok;            /**< PSRAM detected and >= 8 MB (OPTIONAL) */
    size_t   psram_size;          /**< Detected PSRAM size in bytes */
    bool     display_ok;          /**< ILI9341 init succeeded (ESSENTIAL) */
    bool     input_ok;            /**< ADC + buttons init succeeded (ESSENTIAL) */
    bool     audio_ok;            /**< I2S TX/RX init succeeded (OPTIONAL) */
    bool     sdcard_ok;           /**< SD card mounted at /sdcard (OPTIONAL) */
    uint64_t sdcard_size_bytes;   /**< SD capacity in bytes (0 if absent) */
    bool     joystick_cal_ok;     /**< Joystick resting within ±15% of center */
    int16_t  joy_center_x;        /**< Joystick X normalised value at rest */
    int16_t  joy_center_y;        /**< Joystick Y normalised value at rest */
    bool     mic_data_ok;         /**< INMP441 delivered >= 1 byte (OPTIONAL) */
    bool     recovery_triggered;  /**< Button B was held at boot time */
} atlas_system_diag_t;

/**
 * @brief Execute the Atlas OS bootstrap sequence.
 *
 * Sequence:
 *   1. PSRAM verification (optional — warning only).
 *   2. Display init (essential — returns error on failure).
 *   3. Framebuffer allocation (essential — returns error on OOM).
 *   4. SD card mount (optional — continues without card).
 *   5. Boot splash render.
 *   6. Input init (essential — returns error on failure).
 *   7. Audio init (optional — continues without audio).
 *   8. 100 ms input debounce settling delay.
 *   9. Hardware diagnostics snapshot.
 *  10. Recovery trigger check: if Button B is held, calls screen_recovery_run()
 *      which returns if the user chooses "Continue".
 *
 * @param out_diag  Optional pointer to receive the diagnostic snapshot.
 *                  Always written before returning, even on error.
 * @return ESP_OK if bootstrap completed.
 *         ESP_ERR_* if an essential component (display or input) failed.
 */
esp_err_t atlas_system_init(atlas_system_diag_t *out_diag);

#ifdef __cplusplus
}
#endif
