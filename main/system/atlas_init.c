/**
 * @file atlas_init.c
 * @brief Atlas OS Boot Engine & Hardware Self-Check Diagnostics
 */
#include "atlas_init.h"
#include "atlas_pins.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "atlas_sdcard.h"
#include "atlas_gfx.h"
#include "screen_recovery.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "atlas_init";

esp_err_t atlas_system_init(atlas_system_diag_t *out_diag)
{
    ESP_LOGI(TAG, "=========================================================");
    ESP_LOGI(TAG, " ATLAS OS - SYSTEM BOOTSTRAP & DIAGNOSTIC ENGINE       ");
    ESP_LOGI(TAG, " Architecture: ESP32-S3 (ESP-IDF v5) | 16MB Flash, 8MB PSRAM");
    ESP_LOGI(TAG, "=========================================================");

    atlas_system_diag_t diag;
    memset(&diag, 0, sizeof(diag));

    /* ── 1. PSRAM Verification ───────────────────────────────────────────── */
    diag.psram_size = esp_psram_get_size();
    diag.psram_ok   = (diag.psram_size >= (8 * 1024 * 1024));
    ESP_LOGI(TAG, "[BOOT 1/7] PSRAM Verification: %u MB %s",
             (unsigned int)(diag.psram_size / (1024 * 1024)),
             diag.psram_ok ? "[PASS]" : "[WARN: Expected >= 8MB]");

    /* ── 2. Display Early Init & Canvas Clear ────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 2/7] Initializing ILI9341 SPI Display...");
    esp_err_t disp_ret = atlas_display_init();
    diag.display_ok = (disp_ret == ESP_OK);

    uint16_t *boot_fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!boot_fb) {
        boot_fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }

    if (boot_fb) {
        /* Clear canvas to solid black 0x0000 */
        atlas_gfx_clear(boot_fb, COLOR_BLACK);
        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, boot_fb);
        atlas_display_wait_flush();
    }

    /* ── 3. Storage Mount ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 3/7] Mounting MicroSD Card via shared SPI2 host...");
    esp_err_t sd_ret = atlas_sdcard_mount();
    diag.sdcard_ok = (sd_ret == ESP_OK);
    if (diag.sdcard_ok) {
        uint64_t free_b = 0;
        atlas_sdcard_get_info(&diag.sdcard_size_bytes, &free_b);
        ESP_LOGI(TAG, "           MicroSD Card detected: %llu MB", diag.sdcard_size_bytes / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "           MicroSD Card not present or unformatted (non-critical).");
    }

    /* ── 4. Splash Screen ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 4/7] Rendering Atlas Boot Banner...");
    if (boot_fb) {
        atlas_gfx_clear(boot_fb, COLOR_BLACK);
        atlas_gfx_draw_string_centered(boot_fb, 80,  "ATLAS HANDHELD", COLOR_CYAN, COLOR_BLACK, 2);
        atlas_gfx_draw_string_centered(boot_fb, 115, "R2 Reyansh Edition - v1.0", COLOR_WHITE, COLOR_BLACK, 1);
        atlas_gfx_draw_string_centered(boot_fb, 135, "ESP32-S3 | 16MB Flash | 8MB PSRAM", COLOR_TEXT_MUTED, COLOR_BLACK, 1);
        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, boot_fb);
        atlas_display_wait_flush();
    }

    /* ── 5. Input & Audio Subsystems Init ────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 5/7] Initializing Input & Audio Peripherals...");
    esp_err_t input_ret = atlas_input_init();
    diag.input_ok = (input_ret == ESP_OK);

    esp_err_t audio_ret = atlas_audio_init(44100);
    diag.audio_ok = (audio_ret == ESP_OK);

    /* Allow FreeRTOS tasks 100ms to gather initial multi-sample metrics */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── 6. Component Diagnostics ────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 6/7] Running Hardware Diagnostics & Self-Check...");

    /* Joystick Center Check */
    atlas_input_state_t in_state = atlas_input_get_state();
    diag.joy_center_x = in_state.joy_x;
    diag.joy_center_y = in_state.joy_y;
    diag.joystick_cal_ok = (abs(diag.joy_center_x) <= 15 && abs(diag.joy_center_y) <= 15);

    /* INMP441 Microphone RX reception test (attempt to read 64 samples) */
    int16_t mic_buf[64];
    size_t mic_bytes = 0;
    esp_err_t mic_err = atlas_audio_read(mic_buf, 64, &mic_bytes, pdMS_TO_TICKS(50));
    diag.mic_data_ok = (mic_err == ESP_OK && mic_bytes > 0);

    /* Explicitly bypass output-only components (MAX98357A & ILI9341 write-only) */
    ESP_LOGI(TAG, "---------------- HARDWARE SELF-CHECK REPORT ----------------");
    ESP_LOGI(TAG, " 1. Octal PSRAM        : %s (%u MB)", diag.psram_ok ? "PASS" : "WARN", (unsigned int)(diag.psram_size / (1024*1024)));
    ESP_LOGI(TAG, " 2. MicroSD FAT32 Mount: %s (%s)", diag.sdcard_ok ? "PASS" : "ABSENT", diag.sdcard_ok ? "Mounted /sdcard" : "No Card");
    ESP_LOGI(TAG, " 3. Joystick Center Cal: %s (X:%d, Y:%d | Deadzone ±5%%)", diag.joystick_cal_ok ? "PASS" : "WARN", diag.joy_center_x, diag.joy_center_y);
    ESP_LOGI(TAG, " 4. INMP441 I2S1 RX    : %s (%u bytes captured)", diag.mic_data_ok ? "PASS" : "WARN", (unsigned int)mic_bytes);
    ESP_LOGI(TAG, " 5. MAX98357A Amp (TX) : BYPASS (Output-only, clamped to 50%%)");
    ESP_LOGI(TAG, " 6. ILI9341 TFT (SPI)  : BYPASS (Output-only, DMA 80MHz)");
    ESP_LOGI(TAG, "------------------------------------------------------------");

    /* ── 7. Recovery ROM Trigger ─────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 7/7] Checking Safe-Mode Recovery trigger (Hold Button B / SW2)...");
    atlas_input_state_t final_input = atlas_input_get_state();
    if (final_input.btn_b) {
        ESP_LOGW(TAG, "=========================================================");
        ESP_LOGW(TAG, " [ALERT] Button B held at boot! Triggering RECOVERY ROM! ");
        ESP_LOGW(TAG, "=========================================================");
        diag.recovery_triggered = true;
        if (out_diag) *out_diag = diag;
        screen_recovery_run();
    } else {
        ESP_LOGI(TAG, "Button B not asserted; proceeding to standard Atlas OS Home.");
    }

    if (boot_fb) {
        free(boot_fb);
    }

    if (out_diag) {
        *out_diag = diag;
    }

    return ESP_OK;
}
