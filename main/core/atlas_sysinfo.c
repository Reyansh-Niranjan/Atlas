/**
 * @file atlas_sysinfo.c
 * @brief Read-Only System & Hardware Information API Implementation
 */
#include "atlas_sysinfo.h"
#include "atlas_sdcard.h"
#include <string.h>
#include <stdio.h>
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_timer.h"

esp_err_t atlas_sysinfo_get(atlas_sysinfo_t *out_info)
{
    if (!out_info) return ESP_ERR_INVALID_ARG;

    memset(out_info, 0, sizeof(atlas_sysinfo_t));

    /* 1. Silicon Information */
    esp_chip_info_t chip;
    esp_chip_info(&chip);

    snprintf(out_info->chip_model, sizeof(out_info->chip_model), "ESP32-S3");
    out_info->chip_revision = chip.revision;
    out_info->cpu_cores     = chip.cores;
    out_info->cpu_freq_mhz  = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

    /* 2. Internal SRAM Metrics */
    out_info->free_heap_internal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    out_info->min_free_heap_internal = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);

    /* 3. PSRAM Metrics */
    out_info->total_psram     = esp_psram_get_size();
    out_info->psram_available = (out_info->total_psram > 0);
    if (out_info->psram_available) {
        out_info->free_psram = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    } else {
        out_info->free_psram = 0;
    }

    /* 4. System Uptime */
    out_info->uptime_us  = (uint64_t)esp_timer_get_time();
    out_info->uptime_sec = (uint32_t)(out_info->uptime_us / 1000000ULL);

    /* 5. Storage (SD Card) */
    out_info->sdcard_mounted = atlas_sdcard_is_mounted();
    if (out_info->sdcard_mounted) {
        atlas_sdcard_get_info(&out_info->sdcard_total_bytes, &out_info->sdcard_free_bytes);
    }

    /* 6. Firmware Version */
    out_info->firmware_version = ATLAS_FIRMWARE_VERSION;

    return ESP_OK;
}

