/**
 * @file atlas_onboarding.h
 * @brief Onboarding Lifecycle & Configuration Engine for ATLAS OS
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_ONBOARDING_FILE_PATH     "/sdcard/config/onboarding.json"
#define ATLAS_WIFI_TXT_PATH            "/sdcard/wifi.txt"
#define ATLAS_GEMINI_KEY_TXT_PATH      "/sdcard/gemini_key.txt"

typedef struct {
    bool is_completed;
    bool is_update_reonboard;
    char completed_version[32];
    char current_version[32];
} atlas_onboarding_state_t;

/**
 * @brief Check if onboarding is required.
 * Returns true if onboarding.json is missing/false, or if completed_version != running_version.
 */
bool atlas_onboarding_is_required(void);

/**
 * @brief Query detailed onboarding state (including whether this is a post-OTA update).
 */
esp_err_t atlas_onboarding_get_state(atlas_onboarding_state_t *out_state);

/**
 * @brief Mark onboarding as complete for the running firmware version.
 * Writes to /sdcard/config/onboarding.json and NVS.
 */
esp_err_t atlas_onboarding_mark_complete(void);

/**
 * @brief Invalidate onboarding (forces onboarding to run on next boot).
 */
esp_err_t atlas_onboarding_invalidate(void);

/**
 * @brief Parse /sdcard/wifi.txt to extract SSID and Password.
 *
 * @param out_ssid Buffer to store parsed SSID.
 * @param ssid_len Size of out_ssid.
 * @param out_pass Buffer to store parsed Password.
 * @param pass_len Size of out_pass.
 * @return ESP_OK if valid SSID was found.
 */
esp_err_t atlas_onboarding_parse_wifi_file(char *out_ssid, size_t ssid_len,
                                          char *out_pass, size_t pass_len);

/**
 * @brief Ensure /sdcard/wifi.txt template exists on the SD card.
 */
esp_err_t atlas_onboarding_ensure_wifi_template(void);

/**
 * @brief Write credentials to /sdcard/wifi.txt.
 */
esp_err_t atlas_onboarding_write_wifi_file(const char *ssid, const char *password);

/**
 * @brief Parse /sdcard/gemini_key.txt to extract Gemini API key.
 *
 * @param out_key Buffer to store parsed API key.
 * @param key_len Size of out_key.
 * @return ESP_OK if valid API key was found.
 */
esp_err_t atlas_onboarding_parse_gemini_key(char *out_key, size_t key_len);

/**
 * @brief Write Gemini API key to /sdcard/gemini_key.txt.
 *
 * @param api_key API key string.
 * @return ESP_OK on success.
 */
esp_err_t atlas_onboarding_write_gemini_key(const char *api_key);

/**
 * @brief Populate an empty or freshly inserted MicroSD card with all required
 * system directories, configuration templates (wifi.txt, gemini_key.txt),
 * root storage README.txt guide, and default application packages (Store, Settings,
 * Calc, Notes, Synth, Invaders, Gemini Live) in /sdcard/apps/installed/.
 *
 * Idempotent: Does not overwrite existing user applications or configurations.
 *
 * @return ESP_OK on success.
 */
esp_err_t atlas_onboarding_populate_sdcard(void);

/**
 * @brief Perform a complete Factory Reset:
 * - Erases all NVS flash partitions
 * - Unlinks onboarding.json and wifi.txt from SD card
 * - Reboots device cleanly
 */
void atlas_onboarding_perform_factory_reset(void);

#ifdef __cplusplus
}
#endif

