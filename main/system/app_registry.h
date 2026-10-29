/**
 * @file app_registry.h
 * @brief NVS-Backed Application Registry for Atlas OS
 *
 * Architecture (fix/core-audit-pass-1):
 * -----------------------------------------------
 * ESP32-S3 does not provide a process model.  Two execution modes exist:
 *
 *   1. IN-FIRMWARE DISPATCH  — call a handler compiled into this image.
 *      Identified by a stable APP_ID_* constant and a non-NULL handler.
 *      This is what launcher card presses trigger.
 *
 *   2. FIRMWARE UPDATE       — write a validated Atlas OS image to the
 *      inactive OTA partition and reboot.  Called only via the explicit
 *      atlas_fw_update_from_sd() API after a deliberate user confirmation.
 *      Never triggered by a card press.
 *
 * SD-card .bin execution (third-party native apps) is NOT SUPPORTED.
 * The registry still scans /sdcard/apps/ so the UI can display discovered
 * files, but no arbitrary .bin is ever written to a boot partition.
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

/** Stable launcher card IDs — never change across releases. */
#define APP_ID_PKG_STORE   "pkg_store"
#define APP_ID_SETTINGS    "settings"
#define APP_ID_NOVASLATE   "novaslate"
#define APP_ID_MUSIC       "music"

/**
 * @brief In-firmware application dispatch handler.
 *
 * Called from the main-task context.  Must return normally.
 * Must NOT call esp_restart() or write to any partition.
 */
typedef void (*atlas_app_handler_t)(void);

/**
 * @brief Application descriptor stored in NVS.
 *
 * bin_path is populated only for files discovered on the SD card.
 * It does NOT imply the file is a bootable firmware image.
 * flash_offset / size / is_staged are reserved; unused in this release.
 */
typedef struct {
    char     app_id[32];      /**< Stable unique identifier */
    char     app_name[32];    /**< Human-readable title */
    char     tagline[48];     /**< Short descriptor */
    char     version[16];     /**< Semantic version string */
    char     bin_path[128];   /**< SD-card path if discovered, else "" */
    uint32_t flash_offset;    /**< Reserved — unused in current release */
    uint32_t size;            /**< Reserved — unused in current release */
    bool     is_installed;    /**< true = visible in launcher */
    bool     is_staged;       /**< Reserved — always false in current release */
} atlas_app_entry_t;

/* ── Registry lifecycle ──────────────────────────────────────────────────── */

/**
 * @brief Initialize NVS and seed the four default system apps if empty.
 * @return ESP_OK on success.
 */
esp_err_t app_registry_init(void);

/**
 * @brief Scan /sdcard/apps/ for .bin files and register their metadata.
 *
 * Files are registered for display only; they cannot be launched as
 * standalone firmware.  Returns ESP_OK if no SD card is present.
 */
esp_err_t app_registry_scan_sdcard(void);

/* ── Registry lookup ─────────────────────────────────────────────────────── */

/**
 * @brief Retrieve all registered applications.
 * @param out_entries Buffer to receive descriptors.
 * @param max_entries Capacity of out_entries.
 * @param out_count   Receives number of entries written.
 */
esp_err_t app_registry_get_all(atlas_app_entry_t *out_entries,
                               size_t max_entries, size_t *out_count);

/**
 * @brief Look up a descriptor by stable app_id.
 * @return ESP_OK if found, ESP_ERR_NOT_FOUND otherwise.
 */
esp_err_t app_registry_get(const char *app_id, atlas_app_entry_t *out_entry);

/**
 * @brief Register or update a descriptor.
 */
esp_err_t app_registry_register(const atlas_app_entry_t *entry);

/**
 * @brief Soft-delete: mark uninstalled and remove SD binary if present.
 * @return ESP_OK on success, ESP_ERR_NOT_FOUND if not registered.
 */
esp_err_t app_registry_uninstall(const char *app_id);

/* ── In-firmware app dispatch ────────────────────────────────────────────── */

/**
 * @brief Return the compiled-in handler for app_id, or NULL if unimplemented.
 *
 * Callers must check for NULL and display an honest "not available" message.
 */
atlas_app_handler_t app_registry_get_handler(const char *app_id);

/**
 * @brief Dispatch a compiled-in app by its stable app_id.
 *
 * Never writes to flash.  Never calls esp_restart().
 *
 * @return ESP_OK              — handler called and returned.
 *         ESP_ERR_NOT_SUPPORTED — no handler for this app_id (not implemented yet).
 *         ESP_ERR_NOT_FOUND    — app_id not in registry.
 */
esp_err_t app_registry_launch(const char *app_id);

/* ── Boot partition safety ───────────────────────────────────────────────── */

/**
 * @brief Ensure the boot partition points to Atlas OS (ota_0 / factory).
 *
 * Call once at startup.  Safe if ota_0 is already the boot target.
 */
esp_err_t app_registry_ensure_boot_os(void);

/* ── Explicit firmware update (not launcher-triggered) ───────────────────── */

/**
 * @brief Write a validated Atlas OS image from SD card to the inactive OTA
 *        partition, then reboot.
 *
 * Safety checks before any flash write:
 *   - File exists and is readable.
 *   - File size > 0 and <= target partition size.
 *   - esp_image_verify() passes (valid magic + checksum).
 *   - On any error: esp_ota_abort() called, boot partition unchanged.
 *
 * This function is intentionally NOT reachable from a launcher card press.
 * It must only be called from an explicit "Install Firmware Update" action
 * with a user-visible confirmation step.
 *
 * @param bin_path    Full path to Atlas firmware .bin on SD card.
 * @param progress_cb Optional progress callback (bytes_written, total_bytes).
 * @return ESP_OK triggers esp_restart() after setting boot partition.
 *         Any other code: update aborted, boot partition unchanged.
 */
esp_err_t atlas_fw_update_from_sd(const char *bin_path,
                                  void (*progress_cb)(size_t written, size_t total));

#ifdef __cplusplus
}
#endif
