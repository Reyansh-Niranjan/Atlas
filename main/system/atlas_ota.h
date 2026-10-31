/**
 * @file atlas_ota.h
 * @brief OTA Firmware Update, Partition Safety & Hardware Rollback Engine for ATLAS OS
 *
 * Single Source of Truth for:
 * - Boot & running partition detection and safety checks.
 * - Non-running OTA slot selection (prevents overwriting active firmware).
 * - Pre-flash image validation (magic byte 0xE9, chip ID ESP32-S3).
 * - Safe streaming flash writes with abort on error.
 * - Hardware rollback support via ESP-IDF otadata.
 * - Post-boot validation and cancellation of pending rollback.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_partition.h"
#include "esp_ota_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*atlas_ota_progress_cb_t)(size_t written, size_t total);

typedef struct {
    const esp_partition_t *running;
    const esp_partition_t *boot;
    const esp_partition_t *next_update;
    esp_ota_img_states_t   running_state;
    bool                   rollback_possible;
} atlas_ota_status_t;

/**
 * @brief Query current OTA partition topology and state.
 *
 * @param out_status Pointer to structure receiving current status.
 * @return ESP_OK on success, or ESP_ERR_INVALID_ARG if out_status is NULL.
 */
esp_err_t atlas_ota_get_status(atlas_ota_status_t *out_status);

/**
 * @brief Validate an image binary on the filesystem before touching flash.
 *
 * Checks:
 * 1. File existence and readability.
 * 2. File size >= header size and <= maximum OTA partition size.
 * 3. Header magic byte matches ESP_IMAGE_HEADER_MAGIC (0xE9).
 * 4. Header chip ID matches ESP_CHIP_ID_ESP32S3 (0x0009).
 *
 * @param bin_path Path to image file on SD card or storage.
 * @param out_size Optional pointer to receive total image size.
 * @return ESP_OK if valid; ESP_ERR_NOT_FOUND, ESP_ERR_INVALID_SIZE, or ESP_ERR_INVALID_ARG on failure.
 */
esp_err_t atlas_ota_validate_image_file(const char *bin_path, size_t *out_size);

/**
 * @brief Perform a verified firmware upgrade from an SD card binary.
 *
 * Safety guarantees:
 * - The running partition is NEVER overwritten (asserts next_update != running).
 * - File header and chip ID are validated before touching flash.
 * - On any read or write error, esp_ota_abort() is called immediately;
 *   the boot partition target remains unchanged.
 * - Image integrity (SHA-256 digest / checksum) is verified by esp_ota_end()
 *   before boot partition is changed.
 * - Boot partition is updated ONLY after full validation succeeds.
 * - On success, reboots into the new firmware.
 *
 * @param bin_path    Path to the firmware binary (e.g. /sdcard/update.bin).
 * @param progress_cb Optional callback receiving (bytes_written, total_bytes).
 * @return ESP_OK triggers reboot; returns error code on abort or failure.
 */
esp_err_t atlas_ota_update_from_file(const char *bin_path, atlas_ota_progress_cb_t progress_cb);

/**
 * @brief Validate boot health and cancel pending rollback.
 *
 * Call during system service startup (Stage 7). If running in state
 * ESP_OTA_IMG_PENDING_VERIFY, marks the partition valid so the bootloader
 * does not roll back on subsequent reboots.
 *
 * @return ESP_OK on success.
 */
esp_err_t atlas_ota_validate_boot(void);

/**
 * @brief Trigger an immediate rollback to the previous operational firmware partition.
 *
 * @return ESP_ERR_OTA_ROLLBACK_FAILED if no valid rollback slot exists;
 *         otherwise triggers reboot into previous partition.
 */
esp_err_t atlas_ota_rollback(void);

#ifdef __cplusplus
}
#endif

