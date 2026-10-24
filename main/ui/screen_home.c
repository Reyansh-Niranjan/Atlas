/**
 * @file screen_home.c
 * @brief 2x2 Grid Home Launcher & Application Dispatcher for Atlas OS
 */
#include "screen_home.h"
#include "atlas_gfx.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "app_registry.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "screen_home";

/* ── App Card Descriptor for 2x2 Launcher ────────────────────────────────── */
typedef struct {
    const char *id;
    const char *title;
    const char *tagline;
    const char *action_c_label;
    const char *action_d_label;
} launcher_app_t;

static const launcher_app_t s_launcher_apps[4] = {
    {
        .id             = "pkg_store",
        .title          = "APKG Store",
        .tagline        = "Installer & OTA Hub",
        .action_c_label = "Check Updates",
        .action_d_label = "Manage Repos",
    },
    {
        .id             = "settings",
        .title          = "Settings",
        .tagline        = "Telemetry & Cal",
        .action_c_label = "Calibrate ADC",
        .action_d_label = "System Info",
    },
    {
        .id             = "novaslate",
        .title          = "NovaSlate",
        .tagline        = "Curriculum & Reader",
        .action_c_label = "Sync Library",
        .action_d_label = "Bookmarks",
    },
    {
        .id             = "music",
        .title          = "Music",
        .tagline        = "I2S Audio Player",
        .action_c_label = "Play / Pause",
        .action_d_label = "Playlist",
    }
};

/* ── Audio Feedback Helper ───────────────────────────────────────────────── */
static uint16_t *s_home_fb = NULL;

static void home_staging_progress_cb(size_t written, size_t total)
{
    if (!s_home_fb || total == 0) return;
    int pct = (int)((written * 100) / total);

    atlas_gfx_fill_rect(s_home_fb, 30, 80, 260, 80, COLOR_PANEL_BG);
    atlas_gfx_draw_rect(s_home_fb, 30, 80, 260, 80, COLOR_CYAN);
    atlas_gfx_draw_string_centered(s_home_fb, 92, "STAGING APP TO FLASH...", COLOR_CYAN, 0x0001, 1);

    char pct_str[48];
    snprintf(pct_str, sizeof(pct_str), "%d%% (%u KB / %u KB)", pct, (unsigned int)(written / 1024), (unsigned int)(total / 1024));
    atlas_gfx_draw_string_centered(s_home_fb, 108, pct_str, COLOR_WHITE, 0x0001, 1);

    int bar_w = 220;
    int bar_h = 10;
    int bar_x = 50;
    int bar_y = 130;
    int fill = (bar_w * pct) / 100;
    atlas_gfx_draw_rect(s_home_fb, bar_x, bar_y, bar_w, bar_h, COLOR_BORDER_NORM);
    if (fill > 0) {
        atlas_gfx_fill_rect(s_home_fb, bar_x + 1, bar_y + 1, fill - 2 > 0 ? fill - 2 : 1, bar_h - 2, COLOR_GREEN);
    }

    atlas_display_flush(30, 80, 289, 159, s_home_fb);
    atlas_display_wait_flush();
}

static void play_ui_beep(float freq, int duration_ms)
{
    int num_samples = (44100 * duration_ms) / 1000;
    int16_t beep_buf[128 * 2];
    int period_samples = (int)(44100.0f / freq);
    if (period_samples < 2) period_samples = 2;
    int half_period = period_samples / 2;
    int cur_sample = 0;

    size_t samples_left = num_samples;
    while (samples_left > 0) {
        size_t chunk = samples_left > 128 ? 128 : samples_left;
        for (size_t i = 0; i < chunk; i++) {
            int16_t val = (cur_sample < half_period) ? 6000 : -6000;
            cur_sample = (cur_sample + 1) % period_samples;
            beep_buf[i * 2]     = val;
            beep_buf[i * 2 + 1] = val;
        }
        size_t written = 0;
        atlas_audio_write(beep_buf, chunk * 2, &written, pdMS_TO_TICKS(10));
        samples_left -= chunk;
    }
}

/* ── Primary Launcher Loop ───────────────────────────────────────────────── */
void screen_home_run(void)
{
    ESP_LOGI(TAG, "Launching Atlas 2x2 Home Launcher UI...");

    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ESP_LOGE(TAG, "Failed to allocate home framebuffer!");
        return;
    }

    /* Load dynamic applications from NVS and SD card */
    atlas_app_entry_t reg_apps[4];
    size_t reg_count = 0;
    app_registry_get_all(reg_apps, 4, &reg_count);

    int active_row = 0;
    int active_col = 0;
    bool joy_edge_latched = false;
    bool needs_redraw = true;

    /* Debounce state trackers */
    bool last_btn_a = false;
    bool last_btn_b = false;
    bool last_btn_c = false;
    bool last_btn_d = false;

    /* Temporary Toast Notification */
    char toast_msg[64] = {0};
    int toast_timer = 0;

    const int CARD_W = 145;
    const int CARD_H = 76;
    const int COL_X[2] = {10, 165};
    const int ROW_Y[2] = {34, 118};

    while (true) {
        atlas_input_state_t in = atlas_input_get_state();

        /* 1. Joystick Edge-Detected Navigation */
        if (!joy_edge_latched) {
            if (in.joy_x > 50 && active_col == 0) {
                active_col = 1;
                joy_edge_latched = true;
                needs_redraw = true;
                play_ui_beep(880.0f, 15);
            } else if (in.joy_x < -50 && active_col == 1) {
                active_col = 0;
                joy_edge_latched = true;
                needs_redraw = true;
                play_ui_beep(880.0f, 15);
            }

            if (in.joy_y > 50 && active_row == 0) {
                active_row = 1;
                joy_edge_latched = true;
                needs_redraw = true;
                play_ui_beep(880.0f, 15);
            } else if (in.joy_y < -50 && active_row == 1) {
                active_row = 0;
                joy_edge_latched = true;
                needs_redraw = true;
                play_ui_beep(880.0f, 15);
            }
        } else {
            /* Release latch when stick returns to center deadzone */
            if (abs(in.joy_x) < 25 && abs(in.joy_y) < 25) {
                joy_edge_latched = false;
            }
        }

        int active_idx = active_row * 2 + active_col;
        const launcher_app_t *cur_app = &s_launcher_apps[active_idx];

        /* 2. Button Event Processing */
        /* Button A (SW1): Select / Launch */
        if (in.btn_a && !last_btn_a) {
            play_ui_beep(1200.0f, 40);
            const char *target_id = (active_idx < (int)reg_count) ? reg_apps[active_idx].app_id : cur_app->id;
            const char *target_title = (active_idx < (int)reg_count) ? reg_apps[active_idx].app_name : cur_app->title;

            atlas_app_entry_t entry;
            if (app_registry_get(target_id, &entry) == ESP_OK && (entry.is_staged || strlen(entry.bin_path) > 0)) {
                snprintf(toast_msg, sizeof(toast_msg), "BOOTING: %s", target_title);
                s_home_fb = fb;
                app_registry_stage_and_launch(target_id, home_staging_progress_cb);
            } else {
                snprintf(toast_msg, sizeof(toast_msg), "LAUNCHING: %s", target_title);
                toast_timer = 45; /* 1.5 seconds */
                needs_redraw = true;
                ESP_LOGI(TAG, "Event: Launch target -> %s (%s)", target_title, target_id);
            }
        }

        /* Button B (SW2): Back / Reset Focus */
        if (in.btn_b && !last_btn_b) {
            if (active_row != 0 || active_col != 0) {
                active_row = 0;
                active_col = 0;
            }
            snprintf(toast_msg, sizeof(toast_msg), "FOCUS RESET -> %s", s_launcher_apps[0].title);
            toast_timer = 35;
            needs_redraw = true;
            play_ui_beep(587.33f, 25);
            ESP_LOGI(TAG, "Event: Grid focus reset to origin (Cell 0).");
        }

        /* Button C (SW3): Context Action 1 */
        if (in.btn_c && !last_btn_c) {
            snprintf(toast_msg, sizeof(toast_msg), "ACTION: %s", cur_app->action_c_label);
            toast_timer = 40;
            needs_redraw = true;
            play_ui_beep(783.99f, 30);
            ESP_LOGI(TAG, "Event: Context Action 1 -> [%s] on %s", cur_app->action_c_label, cur_app->id);
        }

        /* Button D (SW4): Context Action 2 */
        if (in.btn_d && !last_btn_d) {
            snprintf(toast_msg, sizeof(toast_msg), "ACTION: %s", cur_app->action_d_label);
            toast_timer = 40;
            needs_redraw = true;
            play_ui_beep(987.77f, 30);
            ESP_LOGI(TAG, "Event: Context Action 2 -> [%s] on %s", cur_app->action_d_label, cur_app->id);
        }

        last_btn_a = in.btn_a;
        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;
        last_btn_d = in.btn_d;

        if (toast_timer > 0) {
            toast_timer--;
            needs_redraw = true;
        }

        /* 3. Render Canvas Only When State Changed */
        if (needs_redraw) {
            needs_redraw = false;

            /* Background */
            atlas_gfx_clear(fb, COLOR_BG_DARK);

            /* Top Header */
            atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
            atlas_gfx_draw_string(fb, 12, 8, "ATLAS OS", COLOR_CYAN, 0x0001, 1);
            atlas_gfx_draw_string(fb, 230, 8, "16MB S3", COLOR_TEXT_MUTED, 0x0001, 1);

            /* 4. Render 2x2 Grid Cards */
            for (int r = 0; r < 2; r++) {
                for (int c = 0; c < 2; c++) {
                    int idx = r * 2 + c;
                    bool is_active = (r == active_row && c == active_col);
                    const char *card_title = s_launcher_apps[idx].title;
                    const char *card_tagline = s_launcher_apps[idx].tagline;

                    if (idx < (int)reg_count && strlen(reg_apps[idx].app_name) > 0) {
                        card_title = reg_apps[idx].app_name;
                        if (reg_apps[idx].is_staged) {
                            card_tagline = "[STAGED - READY]";
                        } else if (strlen(reg_apps[idx].bin_path) > 0) {
                            card_tagline = "[SD CARD BINARY]";
                        } else {
                            card_tagline = reg_apps[idx].tagline;
                        }
                    }

                    atlas_gfx_draw_card(fb, COL_X[c], ROW_Y[r], CARD_W, CARD_H,
                                        card_title,
                                        card_tagline,
                                        is_active);
                }
            }

            /* 5. Dynamic Footer Legend */
            atlas_gfx_fill_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 38, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);

            if (toast_timer > 0) {
                atlas_gfx_draw_string_centered(fb, 214, toast_msg, COLOR_GREEN, 0x0001, 1);
            } else {
                char action_line[64];
                snprintf(action_line, sizeof(action_line), "[C] %-14s [D] %s",
                         cur_app->action_c_label, cur_app->action_d_label);
                atlas_gfx_draw_string(fb, 12, 210, action_line, COLOR_CYAN, 0x0001, 1);

                atlas_gfx_draw_string(fb, 12, 224, "[A] Launch    [B] Focus Reset", COLOR_TEXT_MUTED, 0x0001, 1);
            }

            /* 6. Push Full Framebuffer via Non-Blocking DMA */
            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* Fast 50 Hz poll cadence */
    }

    free(fb);
}
