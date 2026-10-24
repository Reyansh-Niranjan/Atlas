/**
 * @file sdcard.c
 * @brief SD card SPI mount using ESP-IDF v5 sdspi + vfs_fat.
 *
 * Mounts the SD card on the shared SPI bus (SPI2_HOST: MOSI=11, SCK=12, MISO=13, CS=10).
 */
#include "sdcard.h"

#include <string.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "sdcard";

#define SD_GPIO_CS   10
#define SD_SPI_HOST  SPI2_HOST   /* Shared with ILI9341 display */

static sdmmc_card_t *s_card = NULL;

esp_err_t sdcard_init(void)
{
    if (s_card) return ESP_OK;  /* Already mounted */

    /* Ensure SD CS pin is HIGH before attempting communication */
    gpio_set_level(SD_GPIO_CS, 1);
    gpio_pullup_en(SD_GPIO_CS);
    gpio_pullup_en(GPIO_NUM_13); /* MISO */

    /* SPI bus was already initialised by display_init(); add SD card device */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = SD_GPIO_CS;
    slot_config.host_id   = SD_SPI_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 4096,  /* Align reads to SD erase block */
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SD_SPI_HOST;
    host.max_freq_khz = 20000;

    esp_err_t ret = esp_vfs_fat_sdspi_mount(SDCARD_MOUNT_POINT,
                                            &host,
                                            &slot_config,
                                            &mount_cfg,
                                            &s_card);
    if (ret != ESP_OK) {
        s_card = NULL;
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "FAT32 SD Card mounted successfully at %s", SDCARD_MOUNT_POINT);
    return ESP_OK;
}

void sdcard_deinit(void)
{
    if (!s_card) return;
    esp_vfs_fat_sdcard_unmount(SDCARD_MOUNT_POINT, s_card);
    s_card = NULL;
    ESP_LOGI(TAG, "SD Card unmounted");
}

FILE *sdcard_open(const char *rel_path, const char *mode)
{
    char full[160];
    if (strncmp(rel_path, SDCARD_MOUNT_POINT, strlen(SDCARD_MOUNT_POINT)) == 0) {
        /* Path already starts with "/sdcard" */
        snprintf(full, sizeof(full), "%s", rel_path);
    } else if (rel_path[0] == '/') {
        snprintf(full, sizeof(full), "%s%s", SDCARD_MOUNT_POINT, rel_path);
    } else {
        snprintf(full, sizeof(full), "%s/%s", SDCARD_MOUNT_POINT, rel_path);
    }

    FILE *f = fopen(full, mode);
    if (!f) {
        ESP_LOGE(TAG, "Failed to open file: %s", full);
    } else {
        ESP_LOGI(TAG, "Successfully opened: %s", full);
    }
    return f;
}
