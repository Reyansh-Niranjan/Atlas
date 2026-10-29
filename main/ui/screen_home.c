/**
 * @file screen_home.c
 * @brief 2x2 Grid Home Launcher for Atlas OS
 *
 * Each card maps to a STABLE app_id constant (APP_ID_*), never to a
 * positional NVS index.  NVS reordering cannot redirect a card to a
 * different app.
 *
 * Launch behaviour:
 *   - app_registry_launch() is called with the card''s explicit app_id.
 *   - ESP_OK   → handler ran; UI returns to home loop.
 *   - ESP_ERR_NOT_SUPPORTED → show "Not implemented" toast.
 *   - any other error  → show truthful error toast.
 *   - The UI NEVER shows a success message unless a handler actually ran.
 *
 * Context actions (C/D) are visually labelled but explicitly marked as
 * not-implemented in this release.  They display an honest message.
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

/* ── Card Descriptor ─────────────────────────────────────────────────────── */
/*
 * Each card''s .id is an APP_ID_* constant.  These are the canonical launch
 * targets and must match what the registry and handlers use.
 * The four positions in this array correspond to grid cells 0-3 (row-major).
 */
typedef struct {
    const char *id;             /**< Stable app_id — authoritative launch key */
    const char *title;          /**< Fallback display title if NVS not available */
    const char *tagline;        /**< Fallback tagline */
    const char *action_c_label; /**< Label for Button C action */
    const char *action_d_label; /**< Label for Button D action */
} launcher_card_t;

static const launcher_card_t s_cards[4] = {
    {
        .id             = APP_ID_PKG_STORE,
        .title          = "APKG Store",
        .tagline        = "Installer & OTA Hub",
        .action_c_label = "Check Updates",   /* not implemented */
        .action_d_label = "Manage Repos",    /* not implemented */
    },
    {
        .id             = APP_ID_SETTINGS,
        .title          = "Settings",
        .tagline        = "Telemetry & Cal",
        .action_c_label = "Calibrate ADC",   /* not implemented */
        .action_d_label = "System Info",     /* not implemented */
    },
    {
        .id             = APP_ID_NOVASLATE,
        .title          = "NovaSlate",
        .tagline        = "Curriculum & Reader",
        .action_c_label = "Sync Library",    /* not implemented */
        .action_d_label = "Bookmarks",       /* not implemented */
    },
    {
        .id             = APP_ID_MUSIC,
        .title          = "Music",
        .tagline        = "I2S Audio Player",
        .action_c_label = "Play / Pause",    /* not implemented */
        .action_d_label = "Playlist",        /* not implemented */
    },
};

/* ── Audio Feedback ──────────────────────────────────────────────────────── */
static void play_ui_beep(float freq, int duration_ms)
{
    int num_samples = (44100 * duration_ms) / 1000;
    int16_t beep_buf[128 * 2];
    int period_samples = (int)(44100.0f / freq);
    if (period_samples < 2) period_samples = 2;
    int half_period = period_samples / 2;
    int cur_sample = 0;

    size_t samples_left = (size_t)num_samples;
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

    uint16_t *fb = (uint16_t *)heap_caps_aligned_alloc(
        64, ATLAS_LCD_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (!fb) {
        fb = (uint16_t *)heap_caps_aligned_alloc(
            64, ATLAS_LCD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!fb) {
        ESP_LOGE(TAG, "Failed to allocate home framebuffer — cannot render UI.");
        return;
    }

    /* Pre-fetch NVS display names for the four fixed cards.
     * We look up by the card''s stable app_id, not by position. */
    atlas_app_entry_t reg_entry[4];
    bool reg_valid[4] = {false, false, false, false};
    for (int i = 0; i < 4; i++) {
        reg_valid[i] = (app_registry_get(s_cards[i].id, &reg_entry[i]) == ESP_OK);
    }

    int active_row = 0;
    int active_col = 0;
    bool joy_latched  = false;
    bool needs_redraw = true;

    bool last_btn_a = false, last_btn_b = false;
    bool last_btn_c = false, last_btn_d = false;

    char toast_msg[64] = {0};
    int  toast_timer   = 0;

    const int CARD_W = 145;
    const int CARD_H = 76;
    const int COL_X[2] = {10, 165};
    const int ROW_Y[2] = {34, 118};

    while (true) {
        atlas_input_state_t in = atlas_input_get_state();

        /* ── Joystick navigation (edge-detected) ─────────────────────────── */
        if (!joy_latched) {
            if      (in.joy_x >  50 && active_col == 0) { active_col = 1; joy_latched = true; needs_redraw = true; play_ui_beep(880.0f, 15); }
            else if (in.joy_x < -50 && active_col == 1) { active_col = 0; joy_latched = true; needs_redraw = true; play_ui_beep(880.0f, 15); }
            if      (in.joy_y >  50 && active_row == 0) { active_row = 1; joy_latched = true; needs_redraw = true; play_ui_beep(880.0f, 15); }
            else if (in.joy_y < -50 && active_row == 1) { active_row = 0; joy_latched = true; needs_redraw = true; play_ui_beep(880.0f, 15); }
        } else if (abs(in.joy_x) < 25 && abs(in.joy_y) < 25) {
            joy_latched = false;
        }

        int active_idx = active_row * 2 + active_col;
        const launcher_card_t *card = &s_cards[active_idx];

        /* ── Button A: Launch (dispatch to compiled-in handler) ─────────── */
        if (in.btn_a && !last_btn_a) {
            play_ui_beep(1200.0f, 40);

            esp_err_t launch_err = app_registry_launch(card->id);
            needs_redraw = true;

            if (launch_err == ESP_OK) {
                /* Handler ran and returned — home screen resumes. */
                snprintf(toast_msg, sizeof(toast_msg), "RETURNED: %s", card->title);
                toast_timer = 35;
            } else if (launch_err == ESP_ERR_NOT_SUPPORTED) {
                /* No compiled handler — be honest. */
                snprintf(toast_msg, sizeof(toast_msg), "NOT AVAILABLE: %s", card->title);
                toast_timer = 50;
                ESP_LOGW(TAG, "[A] Launch '%s': no handler (not implemented).", card->id);
            } else if (launch_err == ESP_ERR_NOT_FOUND) {
                snprintf(toast_msg, sizeof(toast_msg), "ERROR: %s not in registry", card->id);
                toast_timer = 50;
                ESP_LOGE(TAG, "[A] Launch '%s': not in registry.", card->id);
            } else {
                snprintf(toast_msg, sizeof(toast_msg), "ERROR: %s", esp_err_to_name(launch_err));
                toast_timer = 50;
            }
        }

        /* ── Button B: Reset focus ───────────────────────────────────────── */
        if (in.btn_b && !last_btn_b) {
            if (active_row != 0 || active_col != 0) {
                active_row = 0;
                active_col = 0;
            }
            snprintf(toast_msg, sizeof(toast_msg), "FOCUS: %s", s_cards[0].title);
            toast_timer = 30;
            needs_redraw = true;
            play_ui_beep(587.33f, 25);
        }

        /* ── Button C/D: Context actions (not implemented in this release) ── */
        if (in.btn_c && !last_btn_c) {
            play_ui_beep(783.99f, 30);
            snprintf(toast_msg, sizeof(toast_msg), "NOT AVAILABLE: %s", card->action_c_label);
            toast_timer = 40;
            needs_redraw = true;
            ESP_LOGW(TAG, "[C] '%s' on '%s': not implemented.", card->action_c_label, card->id);
        }
        if (in.btn_d && !last_btn_d) {
            play_ui_beep(987.77f, 30);
            snprintf(toast_msg, sizeof(toast_msg), "NOT AVAILABLE: %s", card->action_d_label);
            toast_timer = 40;
            needs_redraw = true;
            ESP_LOGW(TAG, "[D] '%s' on '%s': not implemented.", card->action_d_label, card->id);
        }

        last_btn_a = in.btn_a;
        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;
        last_btn_d = in.btn_d;

        if (toast_timer > 0) {
            toast_timer--;
            needs_redraw = true;
        }

        /* ── Render (only on state change) ──────────────────────────────── */
        if (needs_redraw) {
            needs_redraw = false;

            atlas_gfx_clear(fb, COLOR_BG_DARK);

            /* Header */
            atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
            atlas_gfx_draw_string(fb, 12, 8, "ATLAS OS", COLOR_CYAN, 0x0001, 1);
            atlas_gfx_draw_string(fb, 230, 8, "16MB S3", COLOR_TEXT_MUTED, 0x0001, 1);

            /* 2x2 Cards */
            for (int r = 0; r < 2; r++) {
                for (int c = 0; c < 2; c++) {
                    int idx = r * 2 + c;
                    bool is_active = (r == active_row && c == active_col);

                    /* Display name from NVS if available, else static fallback */
                    const char *title   = s_cards[idx].title;
                    const char *tagline = s_cards[idx].tagline;
                    if (reg_valid[idx] && strlen(reg_entry[idx].app_name) > 0) {
                        title = reg_entry[idx].app_name;
                        if (strlen(reg_entry[idx].tagline) > 0) {
                            tagline = reg_entry[idx].tagline;
                        }
                    }
                    /* If the SD scan gave this card a bin_path, annotate it */
                    if (reg_valid[idx] && strlen(reg_entry[idx].bin_path) > 0) {
                        tagline = "[SD FILE - not launchable]";
                    }

                    atlas_gfx_draw_card(fb, COL_X[c], ROW_Y[r], CARD_W, CARD_H,
                                        title, tagline, is_active);
                }
            }

            /* Footer */
            atlas_gfx_fill_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 38, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);

            /* Re-resolve active card for footer (index may have changed) */
            card = &s_cards[active_row * 2 + active_col];

            if (toast_timer > 0) {
                uint16_t toast_color = COLOR_GREEN;
                /* Distinguish error/unavailable toasts from informational ones */
                if (strncmp(toast_msg, "NOT AVAILABLE", 13) == 0 ||
                    strncmp(toast_msg, "ERROR",         5) == 0) {
                    toast_color = COLOR_YELLOW;
                }
                atlas_gfx_draw_string_centered(fb, 214, toast_msg, toast_color, 0x0001, 1);
            } else {
                char action_line[64];
                snprintf(action_line, sizeof(action_line), "[C] %-14s [D] %s",
                         card->action_c_label, card->action_d_label);
                atlas_gfx_draw_string(fb, 12, 210, action_line, COLOR_CYAN,       0x0001, 1);
                atlas_gfx_draw_string(fb, 12, 224, "[A] Launch    [B] Focus Reset",
                                      COLOR_TEXT_MUTED, 0x0001, 1);
            }

            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* 50 Hz poll */
    }

    /* Unreachable; home loop runs forever. */
    free(fb);
}
