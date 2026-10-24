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
    char     app_id[32];      /**< Unique alphanumeric identifier (e.g. "pkg_store") */
    char     app_name[32];    /**< Human-readable title (e.g. "APKG Store") */
    char     tagline[48];     /**< Short descriptor/subtitle (e.g. "Installer & OTA Hub") */
    char     version[16];     /**< Semantic version string (e.g. "1.0.0") */
    uint32_t flash_offset;    /**< Flash execution partition offset (e.g. ota_1 offset) */
    uint32_t size;            /**< Binary payload size in bytes */
    bool     is_installed;    /**< 1 = active & executable, 0 = ghost deleted / uninstalled */
} atlas_app_entry_t;

/**
 * @brief Initialize NVS storage and seed default system apps if table is empty.
 *
 * @return ESP_OK on success.
 */
esp_err_t app_registry_init(void);

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
 * @brief Fast 1-click uninstall engine.
 * 1. Recursively wipes `/sdcard/apps/<app_id>/`.
 * 2. Clears is_installed flag in NVS.
 * 3. Returns immediately without triggering physical SPI flash sector erases.
 *
 * @param app_id Unique app identifier to remove.
 * @return ESP_OK on success.
 */
esp_err_t app_registry_uninstall(const char *app_id);

#ifdef __cplusplus
}
#endif
