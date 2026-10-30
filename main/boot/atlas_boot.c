/**
 * @file atlas_boot.c
 * @brief Structured Boot Manager Implementation for ATLAS OS
 */
#include "atlas_boot.h"
#include "atlas_pins.h"
#include "atlas_log.h"
#include "atlas_event.h"
#include "atlas_service.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "atlas_sdcard.h"
#include "atlas_gfx.h"
#include "app_registry.h"
#include "screen_home.h"
#include "screen_recovery.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include "esp_psram.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static atlas_boot_stage_t  s_current_stage = ATLAS_BOOT_STAGE_UNINITIALIZED;
static atlas_system_diag_t s_diag_snapshot;

static void boot_advance_stage(atlas_boot_stage_t new_stage, const char *name, esp_err_t status)
{
    s_current_stage = new_stage;
    atlas_event_boot_stage_t evt = {
        .stage_id   = (int32_t)new_stage,
        .stage_name = name,
        .status     = status
    };
    atlas_event_post(ATLAS_EVENT_BOOT_STAGE_ADVANCE, &evt, sizeof(evt), 0);
}

/* ── Stage 1: Core Subsystems ────────────────────────────────────────────── */
static esp_err_t boot_stage_core(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 1/7] Initializing Core (Event Bus & Service Registry)...");
    esp_err_t ret = atlas_event_init();
    if (ret != ESP_OK) return ret;

    ret = atlas_service_registry_init();
    if (ret != ESP_OK) return ret;

    boot_advance_stage(ATLAS_BOOT_STAGE_CORE, "core", ESP_OK);
    return ESP_OK;
}

/* ── Stage 2: Hardware & Display ─────────────────────────────────────────── */
static esp_err_t boot_stage_hardware(uint16_t **out_fb)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 2/7] Initializing Display & PSRAM...");

    /* PSRAM verification (OPTIONAL) */
    s_diag_snapshot.psram_size = esp_psram_get_size();
    s_diag_snapshot.psram_ok   = (s_diag_snapshot.psram_size >= (8 * 1024 * 1024));
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "PSRAM: %u MB %s",
                   (unsigned int)(s_diag_snapshot.psram_size / (1024 * 1024)),
                   s_diag_snapshot.psram_ok ? "[PASS]" : "[WARN: expected >= 8 MB]");

    /* Display Init (ESSENTIAL) */
    esp_err_t disp_ret = atlas_display_init();
    s_diag_snapshot.display_ok = (disp_ret == ESP_OK);
    if (!s_diag_snapshot.display_ok) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_BOOT, "Display init FAILED (%s) — cannot continue.",
                        esp_err_to_name(disp_ret));
        return disp_ret;
    }

    /* Allocate boot framebuffer */
    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(
        64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(
            64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_BOOT, "Boot framebuffer allocation failed — OOM.");
        return ESP_ERR_NO_MEM;
    }

    atlas_gfx_clear(fb, COLOR_BLACK);
    atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
    atlas_display_wait_flush();

    *out_fb = fb;
    boot_advance_stage(ATLAS_BOOT_STAGE_HARDWARE, "hardware", ESP_OK);
    return ESP_OK;
}

/* ── Stage 3: Storage ────────────────────────────────────────────────────── */
static void boot_stage_storage(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 3/7] Mounting MicroSD Card...");
    esp_err_t sd_ret = atlas_sdcard_mount();
    s_diag_snapshot.sdcard_ok = (sd_ret == ESP_OK);

    if (s_diag_snapshot.sdcard_ok) {
        uint64_t free_b = 0;
        atlas_sdcard_get_info(&s_diag_snapshot.sdcard_size_bytes, &free_b);
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "MicroSD FAT32: %llu MB",
                       s_diag_snapshot.sdcard_size_bytes / (1024 * 1024));

        /* Ensure standardized directory structure exists on card */
        const char *std_dirs[] = {
            "/sdcard/system",
            "/sdcard/system/firmware",
            "/sdcard/system/recovery",
            "/sdcard/apps",
            "/sdcard/apps/installed",
            "/sdcard/packages",
            "/sdcard/packages/downloads",
            "/sdcard/data",
            "/sdcard/config",
            "/sdcard/cache",
            "/sdcard/logs"
        };
        for (size_t i = 0; i < sizeof(std_dirs) / sizeof(std_dirs[0]); i++) {
            mkdir(std_dirs[i], 0777);
        }

        atlas_event_storage_t evt = {
            .mount_point = ATLAS_SD_MOUNT_POINT,
            .total_bytes = s_diag_snapshot.sdcard_size_bytes,
            .free_bytes  = free_b,
            .success     = true
        };
        atlas_event_post(ATLAS_EVENT_STORAGE_MOUNTED, &evt, sizeof(evt), 0);
    } else {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_BOOT, "MicroSD absent or unformatted (non-critical).");
        atlas_event_storage_t evt = {
            .mount_point = ATLAS_SD_MOUNT_POINT,
            .total_bytes = 0,
            .free_bytes  = 0,
            .success     = false
        };
        atlas_event_post(ATLAS_EVENT_STORAGE_ERROR, &evt, sizeof(evt), 0);
    }

    boot_advance_stage(ATLAS_BOOT_STAGE_STORAGE, "storage", sd_ret);
}

/* ── Stage 4: Boot Splash ────────────────────────────────────────────────── */
static void boot_stage_splash(uint16_t *boot_fb)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "Rendering boot splash...");
    atlas_gfx_clear(boot_fb, COLOR_BLACK);
    atlas_gfx_draw_string_centered(boot_fb, 80,  "ATLAS HANDHELD",            COLOR_CYAN,       COLOR_BLACK, 2);
    atlas_gfx_draw_string_centered(boot_fb, 115, "R2 Reyansh Edition - v1.0", COLOR_WHITE,      COLOR_BLACK, 1);
    atlas_gfx_draw_string_centered(boot_fb, 135, "ESP32-S3 | 16MB Flash | 8MB PSRAM",
                                   COLOR_TEXT_MUTED, COLOR_BLACK, 1);
    atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, boot_fb);
    atlas_display_wait_flush();
}

/* ── Stage 5: Peripherals (Input & Audio) ────────────────────────────────── */
static esp_err_t boot_stage_peripherals(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 4/7] Initializing Input & Audio Peripherals...");

    /* Input (ESSENTIAL) */
    esp_err_t in_ret = atlas_input_init();
    s_diag_snapshot.input_ok = (in_ret == ESP_OK);
    if (!s_diag_snapshot.input_ok) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_BOOT, "Input init FAILED (%s) — buttons unavailable.",
                        esp_err_to_name(in_ret));
        return in_ret;
    }
    atlas_service_register("input", "Hardware Input Subsystem", NULL, NULL, NULL);

    /* Audio (OPTIONAL) */
    esp_err_t aud_ret = atlas_audio_init(44100);
    s_diag_snapshot.audio_ok = (aud_ret == ESP_OK);
    if (!s_diag_snapshot.audio_ok) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_BOOT, "Audio init failed (%s) — continuing without audio.",
                       esp_err_to_name(aud_ret));
    } else {
        atlas_service_register("audio", "MAX98357A / INMP441 I2S Subsystem", NULL, NULL, NULL);
    }

    /* 100 ms debounce window settling delay */
    vTaskDelay(pdMS_TO_TICKS(100));

    boot_advance_stage(ATLAS_BOOT_STAGE_PERIPHERALS, "peripherals", ESP_OK);
    return ESP_OK;
}

/* ── Stage 6: Diagnostics ────────────────────────────────────────────────── */
static void boot_stage_diagnostics(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 5/7] Executing Hardware Diagnostics...");

    atlas_input_state_t in_state = atlas_input_get_state();
    s_diag_snapshot.joy_center_x    = in_state.joy_x;
    s_diag_snapshot.joy_center_y    = in_state.joy_y;
    s_diag_snapshot.joystick_cal_ok = (abs(s_diag_snapshot.joy_center_x) <= 15 &&
                                       abs(s_diag_snapshot.joy_center_y) <= 15);

    /* Mic RX test (OPTIONAL) */
    size_t mic_bytes = 0;
    if (s_diag_snapshot.audio_ok) {
        int16_t mic_buf[64];
        esp_err_t mic_err = atlas_audio_read(mic_buf, 64, &mic_bytes, pdMS_TO_TICKS(50));
        s_diag_snapshot.mic_data_ok = (mic_err == ESP_OK && mic_bytes > 0);
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "---------------- HARDWARE SELF-CHECK REPORT ----------------");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 1. Octal PSRAM        : %s (%u MB)",
                   s_diag_snapshot.psram_ok ? "PASS" : "WARN",
                   (unsigned int)(s_diag_snapshot.psram_size / (1024 * 1024)));
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 2. ILI9341 Display    : %s", s_diag_snapshot.display_ok ? "PASS" : "FAIL");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 3. MicroSD FAT32      : %s", s_diag_snapshot.sdcard_ok  ? "PASS (mounted)" : "ABSENT");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 4. Input (ADC+Buttons): %s", s_diag_snapshot.input_ok   ? "PASS" : "FAIL");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 5. Audio I2S TX       : %s", s_diag_snapshot.audio_ok   ? "PASS" : "WARN");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 6. Joystick Center    : %s (X:%d Y:%d)",
                   s_diag_snapshot.joystick_cal_ok ? "PASS" : "WARN",
                   s_diag_snapshot.joy_center_x, s_diag_snapshot.joy_center_y);
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " 7. INMP441 Mic RX     : %s (%u bytes)",
                   s_diag_snapshot.mic_data_ok ? "PASS" : "WARN", (unsigned int)mic_bytes);
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "------------------------------------------------------------");

    boot_advance_stage(ATLAS_BOOT_STAGE_DIAGNOSTICS, "diagnostics", ESP_OK);
}

/* ── Stage 7: Recovery Trigger Check ─────────────────────────────────────── */
static bool boot_stage_recovery_check(uint16_t *boot_fb)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 6/7] Checking Recovery Trigger (hold Button B / SW2)...");
    atlas_input_state_t final_in = atlas_input_get_state();
    if (final_in.btn_b) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_BOOT, "=========================================================");
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_BOOT, " [ALERT] Button B held — entering Recovery Safe Mode.   ");
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_BOOT, "=========================================================");
        s_diag_snapshot.recovery_triggered = true;
        if (boot_fb) free(boot_fb);

        boot_advance_stage(ATLAS_BOOT_STAGE_RECOVERY, "recovery", ESP_OK);
        screen_recovery_run(); /* returns if user selects "Continue" */
        return true;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "Button B not held; proceeding to system services.");
    return false;
}

/* ── Stage 8: System Services ────────────────────────────────────────────── */
static esp_err_t boot_stage_services(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "[STAGE 7/7] Starting System Services...");

    /* Application Registry */
    esp_err_t reg_err = app_registry_init();
    if (reg_err != ESP_OK) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_BOOT, "App registry init failed: %s", esp_err_to_name(reg_err));
        return reg_err;
    }

    app_registry_ensure_boot_os();
    app_registry_scan_sdcard();

    atlas_service_register("app_registry", "NVS Application Registry & Dispatcher", NULL, NULL, NULL);

    boot_advance_stage(ATLAS_BOOT_STAGE_SERVICES, "services", ESP_OK);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t atlas_boot_run(atlas_system_diag_t *out_diag)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "=========================================================");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " ATLAS OS - STRUCTURED BOOT MANAGER                     ");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, " Architecture: ESP32-S3 | Staged Lifecycle Engine       ");
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "=========================================================");

    memset(&s_diag_snapshot, 0, sizeof(s_diag_snapshot));
    uint16_t *boot_fb = NULL;

    /* 1. Core Services */
    esp_err_t ret = boot_stage_core();
    if (ret != ESP_OK) return ret;

    /* 2. Hardware & Display */
    ret = boot_stage_hardware(&boot_fb);
    if (ret != ESP_OK) {
        if (out_diag) *out_diag = s_diag_snapshot;
        return ret;
    }

    /* 3. Storage */
    boot_stage_storage();

    /* 4. Boot Splash */
    boot_stage_splash(boot_fb);

    /* 5. Peripherals */
    ret = boot_stage_peripherals();
    if (ret != ESP_OK) {
        if (boot_fb) free(boot_fb);
        if (out_diag) *out_diag = s_diag_snapshot;
        return ret;
    }

    /* 6. Diagnostics */
    boot_stage_diagnostics();

    /* 7. Recovery Check */
    bool recovery_ran = boot_stage_recovery_check(boot_fb);
    if (!recovery_ran && boot_fb) {
        free(boot_fb);
        boot_fb = NULL;
    }

    /* 8. System Services */
    ret = boot_stage_services();
    if (ret != ESP_OK) {
        if (out_diag) *out_diag = s_diag_snapshot;
        return ret;
    }

    boot_advance_stage(ATLAS_BOOT_STAGE_COMPLETE, "complete", ESP_OK);
    atlas_event_post(ATLAS_EVENT_BOOT_COMPLETED, NULL, 0, 0);

    if (out_diag) *out_diag = s_diag_snapshot;
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "ATLAS OS Boot Sequence Completed Successfully.");
    return ESP_OK;
}

atlas_boot_stage_t atlas_boot_get_stage(void)
{
    return s_current_stage;
}

void atlas_boot_launch_shell(void)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_BOOT, "Handoff to ATLAS Shell (screen_home)...");
    screen_home_run();
}

