/**
 * @file atlas_boot.h
 * @brief Structured Boot Manager for ATLAS OS
 *
 * Decomposes system bootstrap into discrete, observable stages with explicit
 * dependency ordering, accurate failure propagation, and recovery preservation.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Per-component boot diagnostic snapshot.
 *
 * Populated during boot diagnostics regardless of success or failure
 * so callers and the recovery screen can display truthful telemetry.
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

typedef enum {
    ATLAS_BOOT_STAGE_UNINITIALIZED = 0,
    ATLAS_BOOT_STAGE_CORE,         /**< Event loop, Service registry */
    ATLAS_BOOT_STAGE_HARDWARE,     /**< PSRAM, ILI9341 display, Framebuffer */
    ATLAS_BOOT_STAGE_STORAGE,      /**< MicroSD mount & directory hierarchy */
    ATLAS_BOOT_STAGE_PERIPHERALS,  /**< Input buttons/joystick, Audio I2S */
    ATLAS_BOOT_STAGE_DIAGNOSTICS,  /**< Hardware self-check diagnostics */
    ATLAS_BOOT_STAGE_RECOVERY,     /**< Button B recovery safe mode check */
    ATLAS_BOOT_STAGE_SERVICES,     /**< Application registry & system services */
    ATLAS_BOOT_STAGE_COMPLETE,     /**< System ready for shell */
    ATLAS_BOOT_STAGE_MAX
} atlas_boot_stage_t;

/**
 * @brief Execute the full ATLAS OS staged boot sequence.
 *
 * @param out_diag Optional destination for diagnostic snapshot.
 * @return ESP_OK if boot succeeded, or error code on essential failure.
 */
esp_err_t atlas_boot_run(atlas_system_diag_t *out_diag);

/**
 * @brief Retrieve current boot stage.
 */
atlas_boot_stage_t atlas_boot_get_stage(void);

/**
 * @brief Launch the default ATLAS desktop shell / home launcher.
 */
void atlas_boot_launch_shell(void);

#ifdef __cplusplus
}
#endif

