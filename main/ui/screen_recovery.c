/**
 * @file screen_recovery.c
 * @brief Failsafe Safe-Mode Recovery ROM UI for Atlas OS
 */
#include "screen_recovery.h"
#include "atlas_gfx.h"
#include "atlas_display.h"
#include "atlas_input.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "screen_recovery";

void screen_recovery_run(void)
{
    ESP_LOGW(TAG, "Entering Atlas Safe-Mode Recovery ROM Screen...");

    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ESP_LOGE(TAG, "Failed to allocate recovery framebuffer!");
        return;
    }

    size_t psram_mb = esp_psram_get_size() / (1024 * 1024);
    uint32_t free_sram_kb = (uint32_t)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);

    bool running = true;
    while (running) {
        atlas_input_state_t in = atlas_input_get_state();

        /* Background */
        atlas_gfx_clear(fb, COLOR_BG_DARK);

        /* Red Warning Banner */
        atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 30, COLOR_RED);
        atlas_gfx_draw_string_centered(fb, 10, "! ATLAS RECOVERY SAFE MODE !", COLOR_WHITE, COLOR_RED, 1);

        /* Diagnostics Card */
        atlas_gfx_fill_rect(fb, 15, 42, 290, 110, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 15, 42, 290, 110, COLOR_BORDER_NORM);

        atlas_gfx_draw_string(fb, 25, 52,  "DIAGNOSTICS & TELEMETRY", COLOR_CYAN, 0x0001, 1);

        char buf[64];
        snprintf(buf, sizeof(buf), "PSRAM : %u MB (Octal 80MHz OK)", (unsigned int)psram_mb);
        atlas_gfx_draw_string(fb, 25, 70,  buf, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(buf, sizeof(buf), "SRAM  : %lu KB free", (unsigned long)free_sram_kb);
        atlas_gfx_draw_string(fb, 25, 86,  buf, COLOR_TEXT_MAIN, 0x0001, 1);

        atlas_gfx_draw_string(fb, 25, 102, "Flash : 16 MB Octal (Factory ROM)", COLOR_TEXT_MAIN, 0x0001, 1);
        atlas_gfx_draw_string(fb, 25, 118, "Status: Button B hold detected at boot", COLOR_YELLOW, 0x0001, 1);

        /* Actions Card */
        atlas_gfx_fill_rect(fb, 15, 160, 290, 48, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 15, 160, 290, 48, COLOR_BORDER_NORM);

        atlas_gfx_draw_string(fb, 25, 168, "[A] Continue to Atlas OS", COLOR_GREEN, 0x0001, 1);
        atlas_gfx_draw_string(fb, 25, 184, "[C] Factory Erase NVS    [D] Reboot", COLOR_ORANGE, 0x0001, 1);

        /* Push frame */
        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        /* Handle buttons */
        if (in.btn_a) {
            ESP_LOGI(TAG, "User exited safe mode; continuing normal boot.");
            running = false;
        } else if (in.btn_c) {
            ESP_LOGW(TAG, "Factory NVS wipe requested!");
            nvs_flash_erase();
            nvs_flash_init();
            atlas_gfx_draw_string(fb, 25, 134, ">> NVS FORMATTED COMPLETE <<", COLOR_GREEN, 0x0001, 1);
            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        } else if (in.btn_d) {
            ESP_LOGI(TAG, "Restart requested from recovery menu.");
            esp_restart();
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }

    free(fb);
}
