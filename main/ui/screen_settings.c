/**
 * @file screen_settings.c
 * @brief Built-in Settings & Hardware Telemetry Screen Implementation
 */
#include "screen_settings.h"
#include "atlas_gfx.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_sysinfo.h"
#include "atlas_log.h"

#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void screen_settings_run(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Launching Settings application...");

    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(
        64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(
            64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "Failed to allocate settings framebuffer.");
        return;
    }

    bool running = true;
    bool last_btn_b = true; /* start debounced if pressed when entering */
    bool last_btn_c = true;
    int tick = 0;

    atlas_sysinfo_t info;
    atlas_sysinfo_get(&info);

    while (running) {
        atlas_input_state_t in = atlas_input_get_state();

        /* Exit on Button B (SW2) or Button C / Menu (SW3) */
        if ((in.btn_b && !last_btn_b) || (in.btn_c && !last_btn_c)) {
            break;
        }

        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;

        /* Refresh telemetry every 25 ticks (1 second) */
        if (++tick >= 25) {
            tick = 0;
            atlas_sysinfo_get(&info);
        }

        /* ── Render Settings Screen ──────────────────────────────────────── */
        atlas_gfx_clear(fb, COLOR_BG_DARK);

        /* Top Header Bar */
        atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 12, 8, "ATLAS OS // SYSTEM SETTINGS", COLOR_CYAN, 0x0001, 1);
        atlas_gfx_draw_string(fb, 255, 8, "[LIVE]", COLOR_GREEN, 0x0001, 1);

        /* Panel 1: Hardware & Memory */
        atlas_gfx_fill_rect(fb, 12, 32, 296, 92, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 12, 32, 296, 92, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 22, 40, "HARDWARE & MEMORY", COLOR_CYAN, 0x0001, 1);

        char line[64];
        snprintf(line, sizeof(line), "Silicon : %s Rev %lu (%u cores @ %lu MHz)",
                 info.chip_model, (unsigned long)info.chip_revision,
                 info.cpu_cores, (unsigned long)info.cpu_freq_mhz);
        atlas_gfx_draw_string(fb, 22, 56, line, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(line, sizeof(line), "SRAM    : %lu KB free (min: %lu KB)",
                 (unsigned long)(info.free_heap_internal / 1024),
                 (unsigned long)(info.min_free_heap_internal / 1024));
        atlas_gfx_draw_string(fb, 22, 72, line, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(line, sizeof(line), "PSRAM   : %u MB total (%u MB free)",
                 (unsigned int)(info.total_psram / (1024 * 1024)),
                 (unsigned int)(info.free_psram / (1024 * 1024)));
        atlas_gfx_draw_string(fb, 22, 88, line, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(line, sizeof(line), "Uptime  : %lu s | Firmware: %s",
                 (unsigned long)info.uptime_sec, info.firmware_version);
        atlas_gfx_draw_string(fb, 22, 104, line, COLOR_TEXT_MUTED, 0x0001, 1);

        /* Panel 2: Storage & Peripherals */
        atlas_gfx_fill_rect(fb, 12, 130, 296, 68, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 12, 130, 296, 68, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 22, 138, "STORAGE & PERIPHERALS", COLOR_CYAN, 0x0001, 1);

        if (info.sdcard_mounted) {
            snprintf(line, sizeof(line), "SD Card : Mounted FAT32 (%llu MB free)",
                     info.sdcard_free_bytes / (1024 * 1024));
            atlas_gfx_draw_string(fb, 22, 154, line, COLOR_GREEN, 0x0001, 1);
        } else {
            atlas_gfx_draw_string(fb, 22, 154, "SD Card : Absent / Not Mounted", COLOR_YELLOW, 0x0001, 1);
        }

        snprintf(line, sizeof(line), "Joystick: X:%4d  Y:%4d", in.joy_x, in.joy_y);
        atlas_gfx_draw_string(fb, 22, 170, line, COLOR_TEXT_MAIN, 0x0001, 1);

        snprintf(line, sizeof(line), "Buttons : [A:%d B:%d C:%d D:%d]",
                 in.btn_a ? 1 : 0, in.btn_b ? 1 : 0, in.btn_c ? 1 : 0, in.btn_d ? 1 : 0);
        atlas_gfx_draw_string(fb, 175, 170, line, COLOR_CYAN, 0x0001, 1);

        /* Bottom Status & Navigation Bar */
        atlas_gfx_fill_rect(fb, 0, 206, ATLAS_LCD_WIDTH, 34, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 0, 206, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
        atlas_gfx_draw_string_centered(fb, 216, "[B] or [Menu] Back to Launcher", COLOR_YELLOW, 0x0001, 1);

        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        vTaskDelay(pdMS_TO_TICKS(40));
    }

    free(fb);
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Settings application exited.");
}

