/**
 * @file sdcard.h
 * @brief SD card initialisation and raw sector-aligned file streaming helpers.
 *
 * Mounts a FAT32 SD card on the shared SPI bus (MOSI=11, SCK=12, MISO=13,
 * CS=10).  Exposes a thin wrapper around stdio FILE* so callers never have to
 * re-open the bus themselves.
 */
#pragma once
#include <stdio.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Mount point used for every path on the card. */
#define SDCARD_MOUNT_POINT "/sdcard"

/**
 * @brief Initialise SPI host and mount the FAT32 partition.
 *
 * Call once before any sdcard_open / sdcard_read.  Safe to call from either
 * core; internally serialised by the VFS layer.
 *
 * @return ESP_OK on success.
 */
esp_err_t sdcard_init(void);

/**
 * @brief Unmount and free SPI resources.  Call on graceful shutdown only.
 */
void sdcard_deinit(void);

/**
 * @brief Open a file on the SD card.
 *
 * Thin wrapper around fopen so callers stay path-agnostic.
 *
 * @param rel_path  Path relative to mount point, e.g. "dracula.wav".
 * @param mode      fopen mode string, e.g. "rb".
 * @return FILE*    NULL on failure.
 */
FILE *sdcard_open(const char *rel_path, const char *mode);

#ifdef __cplusplus
}
#endif
