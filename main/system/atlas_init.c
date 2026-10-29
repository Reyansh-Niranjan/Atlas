/**
 * @file atlas_init.c
 * @brief Atlas OS Boot Engine & Hardware Self-Check Diagnostics
 *
 * Component classification (fix/core-audit-pass-1):
 *   ESSENTIAL  — failure aborts boot (display, input).
 *   OPTIONAL   — failure is logged but boot continues (SD card, audio, mic).
 *
 * Essential: display and input must succeed because the home launcher and
 * recovery screen both require them.  A device with no display or no input
 * is inoperable and must not silently pretend otherwise.
 *
 * Optional: SD card may be absent (no card inserted).  Audio / mic may fail
 * on hardware without the peripheral populated.  Boot continues; the UI will
 * reflect the absence gracefully.
 *
 * Recovery mode:
 *   - Triggered by Button B held at boot time.
 *   - Detected AFTER atlas_input_init() has had 100 ms to prime the
 *     debounce window (8 samples * 10 ms = 80 ms minimum).
 *   - screen_recovery_run() is called and enters its own loop; it returns
 *     only if the user chooses "Continue to Atlas OS".
 *   - Recovery mode does NOT silently erase NVS; that is a deliberate
 *     user action inside screen_recovery_run().
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

    /* ── 1. PSRAM Verification (OPTIONAL) ────────────────────────────────── */
    diag.psram_size = esp_psram_get_size();
    diag.psram_ok   = (diag.psram_size >= (8 * 1024 * 1024));
    ESP_LOGI(TAG, "[BOOT 1/7] PSRAM: %u MB %s",
             (unsigned int)(diag.psram_size / (1024 * 1024)),
             diag.psram_ok ? "[PASS]" : "[WARN: expected >= 8 MB]");
    /* PSRAM shortage is a warning — the device may still limp along. */

    /* ── 2. Display Init (ESSENTIAL) ─────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 2/7] Initializing ILI9341 SPI display...");
    esp_err_t disp_ret = atlas_display_init();
    diag.display_ok = (disp_ret == ESP_OK);
    if (!diag.display_ok) {
        ESP_LOGE(TAG, "[BOOT 2/7] Display init FAILED (%s) — cannot continue.",
                 esp_err_to_name(disp_ret));
        if (out_diag) *out_diag = diag;
        return disp_ret; /* ESSENTIAL: propagate failure */
    }

    /* Allocate boot framebuffer (DMA-capable SRAM preferred, PSRAM fallback) */
    uint16_t *boot_fb = (uint16_t *)heap_caps_aligned_alloc(
        64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!boot_fb) {
        boot_fb = (uint16_t *)heap_caps_aligned_alloc(
            64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!boot_fb) {
        /* Display is up but we cannot allocate a buffer.
         * This is an OOM condition serious enough to halt. */
        ESP_LOGE(TAG, "Boot framebuffer allocation failed — insufficient memory.");
        if (out_diag) *out_diag = diag;
        return ESP_ERR_NO_MEM;
    }

    /* Clear screen to black */
    atlas_gfx_clear(boot_fb, COLOR_BLACK);
    atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, boot_fb);
    atlas_display_wait_flush();

    /* ── 3. SD Card Mount (OPTIONAL) ─────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 3/7] Mounting MicroSD card...");
    esp_err_t sd_ret = atlas_sdcard_mount();
    diag.sdcard_ok = (sd_ret == ESP_OK);
    if (diag.sdcard_ok) {
        uint64_t free_b = 0;
        atlas_sdcard_get_info(&diag.sdcard_size_bytes, &free_b);
        ESP_LOGI(TAG, "           MicroSD: %llu MB", diag.sdcard_size_bytes / (1024 * 1024));
    } else {
        ESP_LOGW(TAG, "           MicroSD absent or unformatted (non-critical).");
    }

    /* ── 4. Splash Screen ────────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 4/7] Rendering boot splash...");
    atlas_gfx_clear(boot_fb, COLOR_BLACK);
    atlas_gfx_draw_string_centered(boot_fb, 80,  "ATLAS HANDHELD",              COLOR_CYAN,       COLOR_BLACK, 2);
    atlas_gfx_draw_string_centered(boot_fb, 115, "R2 Reyansh Edition - v1.0",   COLOR_WHITE,      COLOR_BLACK, 1);
    atlas_gfx_draw_string_centered(boot_fb, 135, "ESP32-S3 | 16MB Flash | 8MB PSRAM",
                                   COLOR_TEXT_MUTED, COLOR_BLACK, 1);
    atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, boot_fb);
    atlas_display_wait_flush();

    /* ── 5. Input Init (ESSENTIAL) ───────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 5/7] Initializing input & audio peripherals...");
    esp_err_t input_ret = atlas_input_init();
    diag.input_ok = (input_ret == ESP_OK);
    if (!diag.input_ok) {
        ESP_LOGE(TAG, "Input init FAILED (%s) — buttons/joystick unavailable.",
                 esp_err_to_name(input_ret));
        /* ESSENTIAL: without input the device cannot be navigated. */
        free(boot_fb);
        if (out_diag) *out_diag = diag;
        return input_ret;
    }

    /* Audio (OPTIONAL) */
    esp_err_t audio_ret = atlas_audio_init(44100);
    diag.audio_ok = (audio_ret == ESP_OK);
    if (!diag.audio_ok) {
        ESP_LOGW(TAG, "Audio init failed (%s) — continuing without audio.",
                 esp_err_to_name(audio_ret));
    }

    /* Allow the input task 100 ms to fill its debounce window (8 * 10 ms = 80 ms min).
     * The recovery button check below depends on this delay being adequate. */
    vTaskDelay(pdMS_TO_TICKS(100));

    /* ── 6. Hardware Diagnostics ─────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 6/7] Hardware self-check...");

    atlas_input_state_t in_state = atlas_input_get_state();
    diag.joy_center_x   = in_state.joy_x;
    diag.joy_center_y   = in_state.joy_y;
    diag.joystick_cal_ok = (abs(diag.joy_center_x) <= 15 && abs(diag.joy_center_y) <= 15);

    /* Mic RX test (OPTIONAL — INMP441 may not be populated) */
    size_t mic_bytes = 0;
    if (diag.audio_ok) {
        int16_t mic_buf[64];
        esp_err_t mic_err = atlas_audio_read(mic_buf, 64, &mic_bytes, pdMS_TO_TICKS(50));
        diag.mic_data_ok = (mic_err == ESP_OK && mic_bytes > 0);
    }

    ESP_LOGI(TAG, "---------------- HARDWARE SELF-CHECK REPORT ----------------");
    ESP_LOGI(TAG, " 1. Octal PSRAM        : %s (%u MB)",
             diag.psram_ok ? "PASS" : "WARN", (unsigned int)(diag.psram_size / (1024 * 1024)));
    ESP_LOGI(TAG, " 2. ILI9341 Display    : %s", diag.display_ok ? "PASS" : "FAIL");
    ESP_LOGI(TAG, " 3. MicroSD FAT32      : %s", diag.sdcard_ok  ? "PASS (mounted)" : "ABSENT");
    ESP_LOGI(TAG, " 4. Input (ADC+Buttons): %s", diag.input_ok   ? "PASS" : "FAIL");
    ESP_LOGI(TAG, " 5. Audio I2S TX       : %s", diag.audio_ok   ? "PASS" : "WARN");
    ESP_LOGI(TAG, " 6. Joystick Center    : %s (X:%d Y:%d)",
             diag.joystick_cal_ok ? "PASS" : "WARN", diag.joy_center_x, diag.joy_center_y);
    ESP_LOGI(TAG, " 7. INMP441 Mic RX     : %s (%u bytes)",
             diag.mic_data_ok ? "PASS" : "WARN", (unsigned int)mic_bytes);
    ESP_LOGI(TAG, "------------------------------------------------------------");

    /* ── 7. Recovery Trigger ─────────────────────────────────────────────── */
    ESP_LOGI(TAG, "[BOOT 7/7] Checking recovery trigger (hold Button B / SW2)...");
    atlas_input_state_t final_in = atlas_input_get_state();
    if (final_in.btn_b) {
        ESP_LOGW(TAG, "=========================================================");
        ESP_LOGW(TAG, " [ALERT] Button B held — entering Recovery Safe Mode.   ");
        ESP_LOGW(TAG, "=========================================================");
        diag.recovery_triggered = true;
        if (out_diag) *out_diag = diag;
        free(boot_fb);
        screen_recovery_run(); /* returns if user selects "Continue" */
        /* Re-allocate framebuffer for the boot sequence to continue */
        /* (boot_fb was freed; home launcher will allocate its own buffer) */
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Button B not held; proceeding to Atlas OS home.");
    free(boot_fb);

    if (out_diag) *out_diag = diag;
    return ESP_OK;
}
