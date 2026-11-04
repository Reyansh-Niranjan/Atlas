/**
 * @file screen_settings.h
 * @brief Interactive Settings & System Telemetry Application for ATLAS OS
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ATLAS_THEME_CYBERPUNK = 0,
    ATLAS_THEME_MONOCHROME,
    ATLAS_THEME_AMBER_CRT,
    ATLAS_THEME_COUNT
} atlas_theme_t;

typedef struct {
    uint8_t       volume;        /* Master playback volume (0..100) */
    bool          sound_fx;      /* UI feedback beeps enabled */
    atlas_theme_t theme;         /* Active color palette */
} atlas_settings_t;

/**
 * @brief Run the interactive Settings application.
 * Returns when the user chooses to exit (Button B or Menu).
 */
void screen_settings_run(void);

/**
 * @brief Load persistent system settings from NVS flash.
 */
void atlas_settings_load(atlas_settings_t *out_settings);

/**
 * @brief Save persistent system settings to NVS flash.
 */
void atlas_settings_save(const atlas_settings_t *settings);

/**
 * @brief Check if UI sound FX (beeps) are currently enabled.
 */
bool atlas_settings_get_sound_fx(void);

#ifdef __cplusplus
}
#endif
