/**
 * @file screen_recovery.c
 * @brief Atlas OS Recovery Safe Mode UI
 *
 * Changes (fix/core-audit-pass-1):
 *   - Renamed from "Recovery ROM" to "Recovery Safe Mode" — this is RAM-
 *     resident code, not ROM.  The original name was misleading.
 *   - NVS erase now requires a two-step confirmation: press C to arm,
 *     then confirm by pressing C again within a 3-second window.
 *     Single-press erase without confirmation is destructive and removed.
 *   - Button A exits recovery and returns to the normal boot flow.
 *   - Button D reboots the device without changes.
 *   - This function returns normally if the user selects "Continue" (A).
 *     The caller (atlas_init.c) then resumes normal boot.
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

/* Countdown ticks at 40 ms per iteration; 3 s = 75 ticks */
#define CONFIRM_TIMEOUT_TICKS  75

void screen_recovery_run(void)
{
    ESP_LOGW(TAG, "Entering Atlas Recovery Safe Mode...");

    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(
        64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(
            64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ESP_LOGE(TAG, "Failed to allocate recovery framebuffer — rebooting.");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return; /* unreachable */
    }

    size_t   psram_mb     = esp_psram_get_size() / (1024 * 1024);
    uint32_t free_sram_kb = (uint32_t)(heap_caps_get_free_size(
                                MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) / 1024);

    /* Confirmation state for NVS erase */
    bool nvs_erase_armed    = false;
    int  confirm_countdown  = 0;

    bool last_btn_a = false, last_btn_c = false, last_btn_d = false;
    bool running    = true;

    while (running) {
        atlas_input_state_t in = atlas_input_get_state();

        /* ── Background ──────────────────────────────────────────────────── */
        atlas_gfx_clear(fb, COLOR_BG_DARK);

        /* Red warning banner */
        atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 30, COLOR_RED);
        atlas_gfx_draw_string_centered(fb, 10, "! ATLAS RECOVERY SAFE MODE !", COLOR_WHITE, COLOR_RED, 1);

        /* Diagnostics card */
        atlas_gfx_fill_rect(fb, 15, 42, 290, 110, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 15, 42, 290, 110, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 25, 52, "DIAGNOSTICS & TELEMETRY", COLOR_CYAN, 0x0001, 1);

        char buf[64];
        snprintf(buf, sizeof(buf), "PSRAM : %u MB (Octal 80 MHz)", (unsigned int)psram_mb);
        atlas_gfx_draw_string(fb, 25, 70,  buf, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(buf, sizeof(buf), "SRAM  : %lu KB free", (unsigned long)free_sram_kb);
        atlas_gfx_draw_string(fb, 25, 86,  buf, COLOR_TEXT_MAIN, 0x0001, 1);

        atlas_gfx_draw_string(fb, 25, 102, "Flash : 16 MB QIO (Atlas firmware)", COLOR_TEXT_MAIN, 0x0001, 1);
        atlas_gfx_draw_string(fb, 25, 118, "Trigger: Button B held at boot",     COLOR_YELLOW,    0x0001, 1);

        /* Actions card */
        atlas_gfx_fill_rect(fb, 15, 160, 290, 66, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 15, 160, 290, 66, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 25, 168, "[A] Continue to Atlas OS", COLOR_GREEN,  0x0001, 1);
        atlas_gfx_draw_string(fb, 25, 184, "[D] Reboot",               COLOR_CYAN,   0x0001, 1);

        if (!nvs_erase_armed) {
            atlas_gfx_draw_string(fb, 25, 200, "[C] Erase NVS (press again to confirm)",
                                  COLOR_ORANGE, 0x0001, 1);
        } else {
            /* Armed: show countdown and require a second press */
            int secs_left = (confirm_countdown + 24) / 25; /* ceil to seconds at 40 ms */
            snprintf(buf, sizeof(buf), "[C] CONFIRM ERASE NVS (%d s) or wait to cancel", secs_left);
            atlas_gfx_draw_string(fb, 25, 200, buf, COLOR_RED, 0x0001, 1);
        }

        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        /* ── Button handling (edge-triggered) ───────────────────────────── */

        /* A: continue to normal boot */
        if (in.btn_a && !last_btn_a) {
            ESP_LOGI(TAG, "User selected Continue — returning to normal boot.");
            running = false;
        }

        /* C: arm erase on first press, execute on second press while armed */
        if (in.btn_c && !last_btn_c) {
            if (!nvs_erase_armed) {
                nvs_erase_armed   = true;
                confirm_countdown = CONFIRM_TIMEOUT_TICKS;
                ESP_LOGW(TAG, "NVS erase ARMED — press C again within 3 s to confirm.");
            } else {
                /* Second press within window — execute */
                ESP_LOGW(TAG, "NVS erase confirmed by user.");
                nvs_flash_erase();
                nvs_flash_init();

                /* Show confirmation on screen */
                atlas_gfx_draw_string_centered(fb, 134, ">> NVS ERASED — rebooting <<",
                                               COLOR_GREEN, 0x0001, 1);
                atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
                atlas_display_wait_flush();
                vTaskDelay(pdMS_TO_TICKS(1500));
                free(fb);
                esp_restart();
                return; /* unreachable */
            }
        }

        /* D: reboot immediately */
        if (in.btn_d && !last_btn_d) {
            ESP_LOGI(TAG, "Reboot requested from recovery.");
            free(fb);
            esp_restart();
            return; /* unreachable */
        }

        last_btn_a = in.btn_a;
        last_btn_c = in.btn_c;
        last_btn_d = in.btn_d;

        /* Countdown armed erase; cancel if it expires */
        if (nvs_erase_armed) {
            if (confirm_countdown > 0) {
                confirm_countdown--;
            } else {
                nvs_erase_armed = false;
                ESP_LOGI(TAG, "NVS erase confirmation timed out — disarmed.");
            }
        }

        vTaskDelay(pdMS_TO_TICKS(40));
    }

    free(fb);
}
