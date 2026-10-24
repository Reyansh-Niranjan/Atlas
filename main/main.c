/**
 * @file main.c
 * @brief Atlas OS - System Architecture Entry Point & Launcher Bootstrap
 */
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>

#include "esp_log.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "atlas_init.h"
#include "app_registry.h"
#include "screen_home.h"

static const char *TAG = "atlas_main";

void app_main(void)
{
    ESP_LOGI(TAG, "Booting Atlas OS v1.0 (ESP32-S3)...");

    /* 1. Initialize NVS Flash Subsystem */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Execute Atlas System Initialization & Diagnostic Self-Check */
    atlas_system_diag_t diag;
    ESP_ERROR_CHECK(atlas_system_init(&diag));

    /* 3. Initialize NVS Application Registry */
    ESP_ERROR_CHECK(app_registry_init());

    /* 4. Launch Primary 2x2 Grid Home Launcher */
    ESP_LOGI(TAG, "Bootstrap complete. Launching 2x2 Home Interface...");
    screen_home_run();
}
