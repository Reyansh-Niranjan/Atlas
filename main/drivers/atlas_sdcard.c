/**
 * @file atlas_sdcard.c
 * @brief High-Speed SPI MicroSD Storage Driver for Atlas (Shared SPI2 Bus)
 */
#include "atlas_sdcard.h"
#include "atlas_pins.h"

#include <string.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/gpio.h"
#include "sdmmc_cmd.h"

static const char *TAG = "atlas_sdcard";

static sdmmc_card_t *s_card = NULL;
static bool          s_is_mounted = false;

esp_err_t atlas_sdcard_mount(void)
{
    if (s_is_mounted && s_card) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Mounting MicroSD card on shared SPI2 host (CS: %d)...", ATLAS_PIN_SD_CS);

    /* 1. Ensure SD CS pin is HIGH and pull-up enabled to isolate card on bus */
    gpio_config_t cs_cfg = {
        .pin_bit_mask = (1ULL << ATLAS_PIN_SD_CS),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    ESP_ERROR_CHECK(gpio_config(&cs_cfg));
    gpio_set_level(ATLAS_PIN_SD_CS, 1);

    /* Enable pull-up on MISO */
    gpio_pullup_en(ATLAS_PIN_LCD_MISO);

    /* 2. Configure SDSPI device on SPI2_HOST */
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs   = ATLAS_PIN_SD_CS;
    slot_config.host_id   = SPI2_HOST;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files              = 8,
        .allocation_unit_size   = 4096,
    };

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = 20000; /* 20 MHz stable SPI SD clock */

    esp_err_t ret = esp_vfs_fat_sdspi_mount(ATLAS_SD_MOUNT_POINT,
                                            &host,
                                            &slot_config,
                                            &mount_cfg,
                                            &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "Failed to mount MicroSD: %s (0x%x)", esp_err_to_name(ret), ret);
        s_card = NULL;
        s_is_mounted = false;
        return ret;
    }

    s_is_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    ESP_LOGI(TAG, "MicroSD FAT32 filesystem mounted at %s (%llu MB)",
             ATLAS_SD_MOUNT_POINT, ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) / (1024 * 1024));
    return ESP_OK;
}

void atlas_sdcard_unmount(void)
{
    if (!s_is_mounted || !s_card) return;
    esp_vfs_fat_sdcard_unmount(ATLAS_SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_is_mounted = false;
    ESP_LOGI(TAG, "MicroSD card unmounted.");
}

bool atlas_sdcard_is_mounted(void)
{
    return s_is_mounted;
}

esp_err_t atlas_sdcard_get_info(uint64_t *out_total_bytes, uint64_t *out_free_bytes)
{
    if (!s_is_mounted || !s_card) {
        if (out_total_bytes) *out_total_bytes = 0;
        if (out_free_bytes) *out_free_bytes = 0;
        return ESP_ERR_INVALID_STATE;
    }

    FATFS *fs = NULL;
    DWORD free_clusters = 0;
    FRESULT res = f_getfree("0:", &free_clusters, &fs);
    if (res != FR_OK || !fs) {
        if (out_total_bytes) {
            *out_total_bytes = (uint64_t)s_card->csd.capacity * s_card->csd.sector_size;
        }
        if (out_free_bytes) *out_free_bytes = 0;
        return ESP_OK;
    }

    uint64_t total_sectors = (fs->n_fatent - 2) * fs->csize;
    uint64_t free_sectors  = (uint64_t)free_clusters * fs->csize;

    if (out_total_bytes) *out_total_bytes = total_sectors * 512ULL;
    if (out_free_bytes)  *out_free_bytes  = free_sectors * 512ULL;

    return ESP_OK;
}
