/**
 * @file atlas_sysinfo.h
 * @brief Read-Only System & Hardware Information API for ATLAS OS
 *
 * Exposes truthful hardware identification, memory usage, uptime,
 * and storage metrics.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_FIRMWARE_VERSION  "1.0.0-arch"

typedef struct {
    char     chip_model[32];
    uint32_t chip_revision;
    uint8_t  cpu_cores;
    uint32_t cpu_freq_mhz;

    /* SRAM Heap Metrics */
    size_t   free_heap_internal;
    size_t   min_free_heap_internal;

    /* PSRAM Metrics */
    bool     psram_available;
    size_t   total_psram;
    size_t   free_psram;

    /* Uptime */
    uint64_t uptime_us;
    uint32_t uptime_sec;

    /* Storage */
    bool     sdcard_mounted;
    uint64_t sdcard_total_bytes;
    uint64_t sdcard_free_bytes;

    /* Firmware */
    const char *firmware_version;
} atlas_sysinfo_t;

/**
 * @brief Populate a snapshot of system and hardware telemetry.
 *
 * @param out_info Pointer to destination struct.
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out_info is NULL.
 */
esp_err_t atlas_sysinfo_get(atlas_sysinfo_t *out_info);

#ifdef __cplusplus
}
#endif

