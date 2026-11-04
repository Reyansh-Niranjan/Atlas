/**
 * @file atlas_ota.c
 * @brief OTA Firmware Update, Partition Safety & Hardware Rollback Engine Implementation
 */
#include "atlas_ota.h"
#include "atlas_log.h"
#include "esp_app_format.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define OTA_STREAM_CHUNK_SIZE  4096

esp_err_t atlas_ota_get_status(atlas_ota_status_t *out_status)
{
    if (!out_status) return ESP_ERR_INVALID_ARG;
    memset(out_status, 0, sizeof(*out_status));

    out_status->running     = esp_ota_get_running_partition();
    out_status->boot        = esp_ota_get_boot_partition();
    out_status->next_update = esp_ota_get_next_update_partition(NULL);

    if (out_status->running) {
        esp_ota_get_state_partition(out_status->running, &out_status->running_state);
    }

    out_status->rollback_possible = esp_ota_check_rollback_is_possible();
    return ESP_OK;
}

esp_err_t atlas_ota_validate_image_file(const char *bin_path, size_t *out_size)
{
    if (!bin_path || strlen(bin_path) == 0) return ESP_ERR_INVALID_ARG;

    struct stat st;
    if (stat(bin_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "OTA image file not found: %s", bin_path);
        return ESP_ERR_NOT_FOUND;
    }

    if (st.st_size < sizeof(esp_image_header_t)) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "OTA image file too small (%ld bytes)", (long)st.st_size);
        return ESP_ERR_INVALID_SIZE;
    }

    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Cannot open OTA image for reading: %s", bin_path);
        return ESP_ERR_NOT_FOUND;
    }

    esp_image_header_t hdr;
    size_t read_bytes = fread(&hdr, 1, sizeof(hdr), f);
    fclose(f);

    if (read_bytes < sizeof(hdr)) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Incomplete read of image header (%u bytes)", (unsigned int)read_bytes);
        return ESP_ERR_INVALID_SIZE;
    }

    /* Verify ESP image magic byte */
    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Invalid image magic 0x%02X (expected 0x%02X)",
                        hdr.magic, ESP_IMAGE_HEADER_MAGIC);
        return ESP_ERR_INVALID_ARG;
    }

    /* Verify target architecture: ESP32-S3 */
    if (hdr.chip_id != ESP_CHIP_ID_ESP32S3) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Invalid chip ID 0x%04X (expected ESP32-S3 0x%04X)",
                        (unsigned int)hdr.chip_id, ESP_CHIP_ID_ESP32S3);
        return ESP_ERR_INVALID_ARG;
    }

    if (out_size) {
        *out_size = (size_t)st.st_size;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Validated image '%s': %ld bytes, chip ESP32-S3, entry 0x%08lx",
                   bin_path, (long)st.st_size, (unsigned long)hdr.entry_addr);
    return ESP_OK;
}

esp_err_t atlas_ota_update_from_file(const char *bin_path, atlas_ota_progress_cb_t progress_cb)
{
    if (!bin_path || strlen(bin_path) == 0) return ESP_ERR_INVALID_ARG;

    /* 1. Pre-flash validation of file & architecture */
    size_t total_size = 0;
    esp_err_t err = atlas_ota_validate_image_file(bin_path, &total_size);
    if (err != ESP_OK) {
        return err;
    }

    /* 2. Locate inactive OTA target partition */
    const esp_partition_t *running = esp_ota_get_running_partition();
    const esp_partition_t *target  = esp_ota_get_next_update_partition(NULL);

    if (!target) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "No OTA update partition found in partition table");
        return ESP_ERR_NOT_FOUND;
    }

    /* CRITICAL SAFETY: Never overwrite the running partition */
    if (target == running) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Safety assertion failed: target partition '%s' is the RUNNING partition!",
                        target->label);
        return ESP_ERR_INVALID_STATE;
    }

    if (target->type != ESP_PARTITION_TYPE_APP) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Target partition '%s' is not APP type", target->label);
        return ESP_ERR_INVALID_ARG;
    }

    if (total_size > target->size) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Image size (%u bytes) exceeds target '%s' capacity (%lu bytes)",
                        (unsigned int)total_size, target->label, (unsigned long)target->size);
        return ESP_ERR_INVALID_SIZE;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Starting OTA update: flashing %u bytes to inactive slot '%s' (0x%lx)...",
                   (unsigned int)total_size, target->label, (unsigned long)target->address);

    /* 3. Open file for streaming */
    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Failed to reopen file: %s", bin_path);
        return ESP_ERR_NOT_FOUND;
    }

    /* 4. Begin OTA session */
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(target, total_size, &ota_handle);
    if (err != ESP_OK) {
        fclose(f);
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 5. Stream chunks from SD card to flash */
    uint8_t *buf = (uint8_t *)malloc(OTA_STREAM_CHUNK_SIZE);
    if (!buf) {
        esp_ota_abort(ota_handle);
        fclose(f);
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Failed to allocate %d bytes stream buffer", OTA_STREAM_CHUNK_SIZE);
        return ESP_ERR_NO_MEM;
    }

    size_t written = 0;
    while (written < total_size) {
        size_t to_read = OTA_STREAM_CHUNK_SIZE;
        if (written + to_read > total_size) {
            to_read = total_size - written;
        }

        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) {
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Unexpected EOF at %u / %u bytes",
                            (unsigned int)written, (unsigned int)total_size);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "esp_ota_write failed at offset %u: %s",
                            (unsigned int)written, esp_err_to_name(err));
            break;
        }

        written += n;
        if (progress_cb) {
            progress_cb(written, total_size);
        }
    }

    free(buf);
    fclose(f);

    /* On write error, abort OTA and leave boot partition intact */
    if (err != ESP_OK) {
        esp_ota_abort(ota_handle);
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "OTA write aborted — boot partition unchanged.");
        return err;
    }

    /* 6. Validate integrity (checksum & SHA-256 digest) */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "esp_ota_end validation FAILED: %s (corrupt image?)",
                        esp_err_to_name(err));
        return err;
    }

    /* 7. Commit new boot partition */
    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "OTA Update SUCCEEDED! Boot partition set to '%s'. Rebooting...",
                   target->label);

    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
    return ESP_OK;
}

esp_err_t atlas_ota_validate_boot(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "Could not determine running partition");
        return ESP_OK;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Running Atlas OS on partition '%s' (addr 0x%06lx, size %lu KB)",
                   running->label, (unsigned long)running->address, (unsigned long)(running->size / 1024));

    /* Only OTA partitions maintain validation states in otadata */
    if (running->type == ESP_PARTITION_TYPE_APP &&
        (running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_0 ||
         running->subtype == ESP_PARTITION_SUBTYPE_APP_OTA_1)) {

        esp_ota_img_states_t state;
        if (esp_ota_get_state_partition(running, &state) == ESP_OK) {
            if (state == ESP_OTA_IMG_PENDING_VERIFY) {
                ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "First boot of newly upgraded firmware. Confirming boot health...");
                esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
                if (err == ESP_OK) {
                    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Firmware marked VALID — rollback cancelled.");
                } else {
                    ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "Failed to cancel rollback: %s", esp_err_to_name(err));
                }
            } else if (state == ESP_OTA_IMG_VALID) {
                ATLAS_LOG_DEBUG(ATLAS_LOG_TAG_SYS, "Firmware partition is already marked VALID.");
            }
        }
    }

    return ESP_OK;
}

esp_err_t atlas_ota_rollback(void)
{
    if (!esp_ota_check_rollback_is_possible()) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_SYS, "Rollback requested, but no valid fallback partition is available!");
        return ESP_ERR_OTA_ROLLBACK_FAILED;
    }

    ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "Rolling back to previous operational firmware partition and rebooting...");
    vTaskDelay(pdMS_TO_TICKS(100));
    return esp_ota_mark_app_invalid_rollback_and_reboot();
}

