/**
 * @file screen_settings.c
 * @brief Interactive System Settings & Hardware Telemetry Application
 */
#include "screen_settings.h"
#include "atlas_gfx.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "atlas_sysinfo.h"
#include "atlas_log.h"
#include "app_manager.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_heap_caps.h"
#include "esp_ota_ops.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "screen_settings";
#define SETTINGS_NVS_NAMESPACE "atlas_cfg"

static atlas_settings_t s_active_settings = {
    .volume   = 100,
    .sound_fx = true,
    .theme    = ATLAS_THEME_CYBERPUNK
};

/* ── Persistence (NVS) ───────────────────────────────────────────────────── */

void atlas_settings_load(atlas_settings_t *out_settings)
{
    if (!out_settings) return;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        uint8_t vol = 100;
        uint8_t sfx = 1;
        uint8_t thm = 0;

        if (nvs_get_u8(handle, "vol", &vol) == ESP_OK) {
            s_active_settings.volume = (vol > 100) ? 100 : vol;
        }
        if (nvs_get_u8(handle, "sfx", &sfx) == ESP_OK) {
            s_active_settings.sound_fx = (sfx != 0);
        }
        if (nvs_get_u8(handle, "thm", &thm) == ESP_OK) {
            s_active_settings.theme = (thm < ATLAS_THEME_COUNT) ? (atlas_theme_t)thm : ATLAS_THEME_CYBERPUNK;
        }
        nvs_close(handle);
    }

    *out_settings = s_active_settings;
    atlas_audio_set_volume(s_active_settings.volume);
}

void atlas_settings_save(const atlas_settings_t *settings)
{
    if (!settings) return;

    s_active_settings = *settings;
    atlas_audio_set_volume(settings->volume);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(SETTINGS_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        nvs_set_u8(handle, "vol", settings->volume);
        nvs_set_u8(handle, "sfx", settings->sound_fx ? 1 : 0);
        nvs_set_u8(handle, "thm", (uint8_t)settings->theme);
        nvs_commit(handle);
        nvs_close(handle);
        ATLAS_LOG_INFO(TAG, "Settings saved to NVS (Vol:%u, SFX:%d, Theme:%d)",
                       settings->volume, settings->sound_fx, (int)settings->theme);
    }
}

bool atlas_settings_get_sound_fx(void)
{
    return s_active_settings.sound_fx;
}

/* ── Interactive Settings UI Execution ───────────────────────────────────── */

#define SETTINGS_ROW_COUNT 5

void screen_settings_run(void)
{
    ATLAS_LOG_INFO(TAG, "Entering Interactive Settings application...");

    uint16_t *fb = atlas_display_get_framebuffer();
    if (!fb) {
        ATLAS_LOG_ERROR(TAG, "Managed framebuffer unavailable.");
        return;
    }

    atlas_settings_t cfg;
    atlas_settings_load(&cfg);

    int selected_row = 0;
    bool running = true;
    bool last_btn_a = true;
    bool last_btn_b = true;
    bool last_btn_c = true;
    bool joy_latched = false;
    int toast_timer = 0;
    char toast_msg[32] = {0};
    int tick = 0;

    atlas_sysinfo_t info;
    atlas_sysinfo_get(&info);

    const esp_partition_t *running_part = esp_ota_get_running_partition();
    const char *part_name = running_part ? running_part->label : "factory";

    while (running) {
        atlas_input_state_t in = atlas_input_get_state();

        /* Exit on Button B or Menu Button (SW3 / btn_c) */
        if ((in.btn_b && !last_btn_b) || (in.btn_c && !last_btn_c)) {
            if (cfg.sound_fx) {
                atlas_audio_play_beep(600.0f, 25);
            }
            atlas_settings_save(&cfg);
            break;
        }

        /* Joystick Vertical Navigation (Row Selection) */
        if (!joy_latched) {
            if (in.joy_y > 45) {
                if (selected_row < SETTINGS_ROW_COUNT - 1) {
                    selected_row++;
                    joy_latched = true;
                    if (cfg.sound_fx) atlas_audio_play_beep(750.0f, 15);
                }
            } else if (in.joy_y < -45) {
                if (selected_row > 0) {
                    selected_row--;
                    joy_latched = true;
                    if (cfg.sound_fx) atlas_audio_play_beep(750.0f, 15);
                }
            }
        }

        /* Value Adjustment (Horizontal Joystick or Button A) */
        bool btn_a_press = (in.btn_a && !last_btn_a);
        bool joy_left    = (!joy_latched && in.joy_x < -45);
        bool joy_right   = (!joy_latched && in.joy_x > 45);

        if (joy_left || joy_right) {
            joy_latched = true;
        } else if (abs(in.joy_x) < 25 && abs(in.joy_y) < 25) {
            joy_latched = false;
        }

        /* Handle adjustments per row */
        if (selected_row == 0) {
            /* Master Volume: 0%, 25%, 50%, 75%, 100% */
            if (joy_right || (btn_a_press && cfg.volume < 100)) {
                if (cfg.volume <= 75) cfg.volume += 25;
                else cfg.volume = 100;
                atlas_audio_set_volume(cfg.volume);
                if (cfg.sound_fx) atlas_audio_play_beep(880.0f, 25);
                snprintf(toast_msg, sizeof(toast_msg), "VOLUME: %u%%", cfg.volume);
                toast_timer = 25;
            } else if (joy_left || (btn_a_press && cfg.volume == 100)) {
                if (btn_a_press && cfg.volume == 100) cfg.volume = 0;
                else if (cfg.volume >= 25) cfg.volume -= 25;
                else cfg.volume = 0;
                atlas_audio_set_volume(cfg.volume);
                if (cfg.volume > 0 && cfg.sound_fx) atlas_audio_play_beep(880.0f, 25);
                snprintf(toast_msg, sizeof(toast_msg), "VOLUME: %u%%", cfg.volume);
                toast_timer = 25;
            }
        } else if (selected_row == 1) {
            /* Sound FX: ON / OFF */
            if (btn_a_press || joy_left || joy_right) {
                cfg.sound_fx = !cfg.sound_fx;
                if (cfg.sound_fx) atlas_audio_play_beep(1200.0f, 30);
                snprintf(toast_msg, sizeof(toast_msg), "SOUND FX: %s", cfg.sound_fx ? "ON" : "OFF");
                toast_timer = 25;
            }
        } else if (selected_row == 2) {
            /* Display Theme */
            if (btn_a_press || joy_right) {
                cfg.theme = (atlas_theme_t)((cfg.theme + 1) % ATLAS_THEME_COUNT);
                if (cfg.sound_fx) atlas_audio_play_beep(1000.0f, 20);
                snprintf(toast_msg, sizeof(toast_msg), "THEME CHANGED");
                toast_timer = 25;
            } else if (joy_left) {
                cfg.theme = (atlas_theme_t)((cfg.theme + ATLAS_THEME_COUNT - 1) % ATLAS_THEME_COUNT);
                if (cfg.sound_fx) atlas_audio_play_beep(1000.0f, 20);
                snprintf(toast_msg, sizeof(toast_msg), "THEME CHANGED");
                toast_timer = 25;
            }
        } else if (selected_row == 3) {
            /* Storage Rescan */
            if (btn_a_press) {
                app_manager_scan_installed();
                atlas_sysinfo_get(&info);
                if (cfg.sound_fx) atlas_audio_play_beep(1400.0f, 40);
                snprintf(toast_msg, sizeof(toast_msg), "CATALOGUE RESCANNED");
                toast_timer = 35;
            }
        }

        last_btn_a = in.btn_a;
        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;

        /* Periodic telemetry refresh */
        if (++tick >= 25) {
            tick = 0;
            atlas_sysinfo_get(&info);
        }

        /* ── Theme Palette Resolution ────────────────────────────────────── */
        uint16_t col_bg        = COLOR_BG_DARK;
        uint16_t col_panel     = COLOR_PANEL_BG;
        uint16_t col_border    = COLOR_BORDER_NORM;
        uint16_t col_accent    = COLOR_CYAN;
        uint16_t col_text      = COLOR_TEXT_MAIN;
        uint16_t col_highlight = COLOR_BORDER_SEL;

        if (cfg.theme == ATLAS_THEME_MONOCHROME) {
            col_bg        = COLOR_BLACK;
            col_panel     = 0x18C3;
            col_border    = COLOR_TEXT_MUTED;
            col_accent    = COLOR_WHITE;
            col_text      = COLOR_WHITE;
            col_highlight = COLOR_WHITE;
        } else if (cfg.theme == ATLAS_THEME_AMBER_CRT) {
            col_bg        = 0x1080;
            col_panel     = 0x2100;
            col_border    = COLOR_ORANGE;
            col_accent    = COLOR_YELLOW;
            col_text      = COLOR_YELLOW;
            col_highlight = COLOR_ORANGE;
        }

        /* ── Render Display ──────────────────────────────────────────────── */
        atlas_gfx_clear(fb, col_bg);

        /* Top Header Bar */
        atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 24, col_panel);
        atlas_gfx_draw_rect(fb, 0, 23, ATLAS_LCD_WIDTH, 1, col_border);
        atlas_gfx_draw_string(fb, 12, 7, "ATLAS OS // SYSTEM SETTINGS", col_accent, 0x0001, 1);
        atlas_gfx_draw_string(fb, 260, 7, "[LIVE]", COLOR_GREEN, 0x0001, 1);

        /* Main Configuration Panel (Rows 0 to 3) */
        int py = 28;
        int row_h = 24;

        for (int r = 0; r < 4; r++) {
            bool is_sel = (selected_row == r);
            uint16_t r_bg = is_sel ? 0x1A25 : col_panel;
            uint16_t r_bd = is_sel ? col_highlight : col_border;

            atlas_gfx_fill_rect(fb, 10, py, 300, row_h, r_bg);
            atlas_gfx_draw_rect(fb, 10, py, 300, row_h, r_bd);

            if (is_sel) {
                atlas_gfx_draw_string(fb, 14, py + 7, ">", col_accent, 0x0001, 1);
            }

            if (r == 0) {
                atlas_gfx_draw_string(fb, 26, py + 7, "Master Volume", is_sel ? col_accent : col_text, 0x0001, 1);
                /* Graphical volume gauge */
                int bar_w = 60;
                int fill_w = (cfg.volume * bar_w) / 100;
                int bx = 180;
                int by = py + 7;
                atlas_gfx_draw_rect(fb, bx, by, bar_w, 10, col_border);
                if (fill_w > 0) {
                    atlas_gfx_fill_rect(fb, bx + 1, by + 1, fill_w - 2, 8, is_sel ? col_accent : COLOR_GREEN);
                }
                char vstr[12];
                snprintf(vstr, sizeof(vstr), "%3u%%", cfg.volume);
                atlas_gfx_draw_string(fb, 255, py + 7, vstr, is_sel ? col_accent : col_text, 0x0001, 1);
            } else if (r == 1) {
                atlas_gfx_draw_string(fb, 26, py + 7, "UI Sound FX", is_sel ? col_accent : col_text, 0x0001, 1);
                const char *sfx_str = cfg.sound_fx ? "[ ENABLED ]" : "[  MUTED  ]";
                uint16_t sfx_col = cfg.sound_fx ? COLOR_GREEN : COLOR_RED;
                atlas_gfx_draw_string(fb, 215, py + 7, sfx_str, sfx_col, 0x0001, 1);
            } else if (r == 2) {
                atlas_gfx_draw_string(fb, 26, py + 7, "Display Theme", is_sel ? col_accent : col_text, 0x0001, 1);
                const char *thm_str = (cfg.theme == ATLAS_THEME_CYBERPUNK) ? "CYBERPUNK" :
                                      (cfg.theme == ATLAS_THEME_MONOCHROME) ? "MONO B&W" : "AMBER CRT";
                atlas_gfx_draw_string(fb, 215, py + 7, thm_str, is_sel ? col_accent : COLOR_YELLOW, 0x0001, 1);
            } else if (r == 3) {
                atlas_gfx_draw_string(fb, 26, py + 7, "Storage & Apps", is_sel ? col_accent : col_text, 0x0001, 1);
                if (info.sdcard_mounted) {
                    char sd_str[32];
                    snprintf(sd_str, sizeof(sd_str), "SD:%lluMB [A:SCAN]", info.sdcard_free_bytes / (1024 * 1024));
                    atlas_gfx_draw_string(fb, 175, py + 7, sd_str, is_sel ? col_accent : COLOR_GREEN, 0x0001, 1);
                } else {
                    atlas_gfx_draw_string(fb, 195, py + 7, "NO SDCARD", COLOR_YELLOW, 0x0001, 1);
                }
            }

            py += row_h + 3;
        }

        /* Panel 2: Live Telemetry & Hardware Diagnostics (Row 4) */
        bool is_diag_sel = (selected_row == 4);
        atlas_gfx_fill_rect(fb, 10, py, 300, 68, is_diag_sel ? 0x1A25 : col_panel);
        atlas_gfx_draw_rect(fb, 10, py, 300, 68, is_diag_sel ? col_highlight : col_border);
        atlas_gfx_draw_string(fb, 16, py + 6, "SYSTEM HEALTH & DIAGNOSTICS", col_accent, 0x0001, 1);

        char line[64];
        snprintf(line, sizeof(line), "CPU: %s Rev %lu (%u cores @ %lu MHz)",
                 info.chip_model, (unsigned long)info.chip_revision,
                 info.cpu_cores, (unsigned long)info.cpu_freq_mhz);
        atlas_gfx_draw_string(fb, 16, py + 20, line, col_text, 0x0001, 1);

        snprintf(line, sizeof(line), "SRAM Free: %lu KB | PSRAM Free: %u MB",
                 (unsigned long)(info.free_heap_internal / 1024),
                 (unsigned int)(info.free_psram / (1024 * 1024)));
        atlas_gfx_draw_string(fb, 16, py + 34, line, col_text, 0x0001, 1);

        snprintf(line, sizeof(line), "OTA Slot : %s | Uptime: %lu s",
                 part_name, (unsigned long)info.uptime_sec);
        atlas_gfx_draw_string(fb, 16, py + 48, line, COLOR_TEXT_MUTED, 0x0001, 1);

        /* Toast Notification Overlay */
        if (toast_timer > 0) {
            toast_timer--;
            atlas_gfx_fill_rect(fb, 60, 95, 200, 26, 0x0841);
            atlas_gfx_draw_rect(fb, 60, 95, 200, 26, col_accent);
            atlas_gfx_draw_string_centered(fb, 103, toast_msg, COLOR_WHITE, 0x0001, 1);
        }

        /* Bottom Controls Bar */
        atlas_gfx_fill_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 26, col_panel);
        atlas_gfx_draw_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 1, col_border);
        atlas_gfx_draw_string(fb, 12, 222, "[B/Menu] Save & Exit", COLOR_YELLOW, 0x0001, 1);
        atlas_gfx_draw_string(fb, 175, 222, "[Joy/A] Adjust Setting", col_accent, 0x0001, 1);

        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        vTaskDelay(pdMS_TO_TICKS(40));
    }

    ATLAS_LOG_INFO(TAG, "Settings application exited cleanly.");
}
