/**
 * @file main.c
 * @brief Atlas OS - System Architecture Entry Point & Structured Boot Bootstrap
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "nvs_flash.h"
#include "atlas_log.h"
#include "atlas_boot.h"

void app_main(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Booting Atlas OS v1.0 (ESP32-S3)...");

    /* 1. Initialize NVS Flash Subsystem */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Execute Structured Boot Sequence */
    atlas_system_diag_t diag;
    ESP_ERROR_CHECK(atlas_boot_run(&diag));

    /* 3. Launch ATLAS Shell */
    atlas_boot_launch_shell();
}
