/**
 * @file atlas_sdcard.h
 * @brief High-Speed SPI MicroSD Storage Driver for Atlas (Shared SPI2 Bus)
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Mount MicroSD card over SPI2_HOST with FatFS at /sdcard.
 *
 * @return ESP_OK if successfully mounted, error code otherwise.
 */
esp_err_t atlas_sdcard_mount(void);

/**
 * @brief Unmount MicroSD card and release FatFS structures.
 */
void atlas_sdcard_unmount(void);

/**
 * @brief Check if MicroSD card is currently mounted and accessible.
 *
 * @return true if mounted.
 */
bool atlas_sdcard_is_mounted(void);

/**
 * @brief Get total and free space on the mounted MicroSD card.
 *
 * @param out_total_bytes Pointer to store total capacity in bytes.
 * @param out_free_bytes  Pointer to store free capacity in bytes.
 * @return ESP_OK on success.
 */
esp_err_t atlas_sdcard_get_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes);

#ifdef __cplusplus
}
#endif
