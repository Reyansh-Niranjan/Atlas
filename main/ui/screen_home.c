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
static void play_ui_beep(float freq, int duration_ms)
{
    int num_samples = (44100 * duration_ms) / 1000;
    int16_t beep_buf[256 * 2];
    float phase = 0.0f;
    float phase_inc = (2.0f * M_PI * freq) / 44100.0f;

    size_t samples_left = num_samples;
    while (samples_left > 0) {
        size_t chunk = samples_left > 256 ? 256 : samples_left;
        for (size_t i = 0; i < chunk; i++) {
            int16_t val = (int16_t)(sinf(phase) * 12000.0f);
            phase += phase_inc;
            if (phase >= 2.0f * M_PI) phase -= 2.0f * M_PI;
            beep_buf[i * 2]     = val;
            beep_buf[i * 2 + 1] = val;
        }
        size_t written = 0;
        atlas_audio_write(beep_buf, chunk * 2, &written, pdMS_TO_TICKS(20));
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

    int active_row = 0;
    int active_col = 0;
    bool joy_edge_latched = false;

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
                play_ui_beep(880.0f, 15);
            } else if (in.joy_x < -50 && active_col == 1) {
                active_col = 0;
                joy_edge_latched = true;
                play_ui_beep(880.0f, 15);
            }

            if (in.joy_y > 50 && active_row == 0) {
                active_row = 1;
                joy_edge_latched = true;
                play_ui_beep(880.0f, 15);
            } else if (in.joy_y < -50 && active_row == 1) {
                active_row = 0;
                joy_edge_latched = true;
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
            snprintf(toast_msg, sizeof(toast_msg), "LAUNCHING: %s", cur_app->title);
            toast_timer = 45; /* 1.5 seconds */
            play_ui_beep(1200.0f, 40);
            ESP_LOGI(TAG, "Event: Launch target -> %s (%s)", cur_app->title, cur_app->id);
        }

        /* Button B (SW2): Back / Reset Focus */
        if (in.btn_b && !last_btn_b) {
            active_row = 0;
            active_col = 0;
            snprintf(toast_msg, sizeof(toast_msg), "FOCUS RESET -> %s", s_launcher_apps[0].title);
            toast_timer = 35;
            play_ui_beep(587.33f, 25);
            ESP_LOGI(TAG, "Event: Grid focus reset to origin (Cell 0).");
        }

        /* Button C (SW3): Context Action 1 */
        if (in.btn_c && !last_btn_c) {
            snprintf(toast_msg, sizeof(toast_msg), "ACTION: %s", cur_app->action_c_label);
            toast_timer = 40;
            play_ui_beep(783.99f, 30);
            ESP_LOGI(TAG, "Event: Context Action 1 -> [%s] on %s", cur_app->action_c_label, cur_app->id);
        }

        /* Button D (SW4): Context Action 2 */
        if (in.btn_d && !last_btn_d) {
            snprintf(toast_msg, sizeof(toast_msg), "ACTION: %s", cur_app->action_d_label);
            toast_timer = 40;
            play_ui_beep(987.77f, 30);
            ESP_LOGI(TAG, "Event: Context Action 2 -> [%s] on %s", cur_app->action_d_label, cur_app->id);
        }

        last_btn_a = in.btn_a;
        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;
        last_btn_d = in.btn_d;

        /* 3. Render Canvas */
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
                atlas_gfx_draw_card(fb, COL_X[c], ROW_Y[r], CARD_W, CARD_H,
                                    s_launcher_apps[idx].title,
                                    s_launcher_apps[idx].tagline,
                                    is_active);
            }
        }

        /* 5. Dynamic Footer Legend */
        atlas_gfx_fill_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 38, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);

        if (toast_timer > 0) {
            /* Active Toast Notification */
            toast_timer--;
            atlas_gfx_draw_string_centered(fb, 214, toast_msg, COLOR_GREEN, 0x0001, 1);
        } else {
            /* Dynamic Context Legend for Active App */
            char action_line[64];
            snprintf(action_line, sizeof(action_line), "[C] %-14s [D] %s",
                     cur_app->action_c_label, cur_app->action_d_label);
            atlas_gfx_draw_string(fb, 12, 210, action_line, COLOR_CYAN, 0x0001, 1);

            atlas_gfx_draw_string(fb, 12, 224, "[A] Launch    [B] Focus Reset", COLOR_TEXT_MUTED, 0x0001, 1);
        }

        /* 6. Push Full Framebuffer via Non-Blocking DMA */
        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        vTaskDelay(pdMS_TO_TICKS(33)); /* ~30 FPS */
    }

    free(fb);
}
