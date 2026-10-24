/**
 * @file app_registry.h
 * @brief NVS-Backed Application Metadata Registry & Fast Deletion Engine for Atlas OS
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define APP_REGISTRY_MAX_APPS  16

/**
 * @brief Application manifest and flash metadata descriptor.
 */
typedef struct {
    char     app_id[32];      /**< Unique alphanumeric identifier (e.g. "doom") */
    char     app_name[32];    /**< Human-readable title (e.g. "Doom Classic") */
    char     tagline[48];     /**< Short descriptor/subtitle (e.g. "Port by Espressif") */
    char     version[16];     /**< Semantic version string (e.g. "1.0.0") */
    char     bin_path[128];   /**< Full path on SD card (e.g. "/sdcard/apps/doom.bin") */
    uint32_t flash_offset;    /**< Flash execution partition offset (ota_1) */
    uint32_t size;            /**< Binary payload size in bytes */
    bool     is_installed;    /**< 1 = active & executable */
    bool     is_staged;       /**< 1 = currently flashed in ota_1 partition ready for instant boot */
} atlas_app_entry_t;

/**
 * @brief Initialize NVS storage and seed default system apps if table is empty.
 *
 * @return ESP_OK on success.
 */
esp_err_t app_registry_init(void);

/**
 * @brief Dynamically scan `/sdcard/apps/` for `.bin` files and register them.
 *
 * @return ESP_OK on success.
 */
esp_err_t app_registry_scan_sdcard(void);

/**
 * @brief Retrieve all registered applications.
 *
 * @param out_entries Array buffer to receive app descriptors.
 * @param max_entries Maximum capacity of out_entries.
 * @param out_count   Pointer to store number of retrieved entries.
 * @return ESP_OK on success.
 */
esp_err_t app_registry_get_all(atlas_app_entry_t *out_entries, size_t max_entries, size_t *out_count);

/**
 * @brief Lookup application metadata by app_id.
 *
 * @param app_id    Unique app identifier.
 * @param out_entry Destination pointer to write app metadata.
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND if missing.
 */
esp_err_t app_registry_get(const char *app_id, atlas_app_entry_t *out_entry);

/**
 * @brief Register or update an application descriptor in the NVS table.
 *
 * @param entry Pointer to valid app descriptor.
 * @return ESP_OK on success.
 */
esp_err_t app_registry_register(const atlas_app_entry_t *entry);

/**
 * @brief Check if an app is currently staged in the ota_1 flash partition.
 *
 * @param app_id App identifier to check.
 * @return true if currently staged in ota_1, false otherwise.
 */
bool app_registry_is_staged(const char *app_id);

/**
 * @brief Stage app into ota_1 flash partition (if not already staged) and reboot into it.
 * Atlas OS in ota_0 remains 100% untouched.
 *
 * @param app_id      Unique identifier of app to launch.
 * @param progress_cb Optional progress callback (written bytes, total bytes) for UI progress bar.
 * @return ESP_OK on success (will trigger esp_restart() immediately).
 */
esp_err_t app_registry_stage_and_launch(const char *app_id, void (*progress_cb)(size_t written, size_t total));

/**
 * @brief Ensure boot partition is set to Atlas OS (ota_0 / factory) for failsafe boot.
 *
 * @return ESP_OK on success.
 */
esp_err_t app_registry_ensure_boot_os(void);

/**
 * @brief Fast 1-click uninstall engine.
 *
 * @param app_id Unique app identifier to remove.
 * @return ESP_OK on success.
 */
esp_err_t app_registry_uninstall(const char *app_id);

#ifdef __cplusplus
}
#endif
