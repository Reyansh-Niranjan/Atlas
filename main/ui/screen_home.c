/**
 * @file screen_home.c
 * @brief Dynamic Grid Home Launcher for ATLAS OS
 *
 * Single source of truth: queries the Application Manager dynamically.
 * Features:
 *   - Renders 2x2 grid of genuinely registered applications.
 *   - Supports pagination when total applications exceed 4.
 *   - Displays empty state when no applications are available.
 *   - Shows SD-card status in the header.
 *   - Safely executes launchable apps and rejects unsupported runtimes with honest UI toasts.
 */
#include "screen_home.h"
#include "atlas_gfx.h"
#include "atlas_display.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "atlas_sdcard.h"
#include "atlas_event.h"
#include "app_manager.h"

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "screen_home";

static volatile bool s_catalogue_dirty = false;

static void on_catalogue_changed(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    s_catalogue_dirty = true;
}

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
    ESP_LOGI(TAG, "Launching Dynamic ATLAS Home Launcher UI...");

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

    /* Subscribe to application catalogue change events */
    atlas_event_register_handler(ATLAS_EVENT_APP_CATALOGUE_CHANGED, on_catalogue_changed, NULL);

    int current_page = 0;
    int active_row   = 0;
    int active_col   = 0;
    bool joy_latched = false;
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
        if (s_catalogue_dirty) {
            s_catalogue_dirty = false;
            needs_redraw = true;
        }

        size_t total_apps = app_manager_get_count();
        int total_pages   = (total_apps == 0) ? 1 : (int)((total_apps + 3) / 4);

        if (current_page >= total_pages) {
            current_page = total_pages - 1;
            needs_redraw = true;
        }

        int page_start = current_page * 4;
        int page_apps  = 0;
        if ((int)total_apps > page_start) {
            page_apps = (int)total_apps - page_start;
            if (page_apps > 4) page_apps = 4;
        }

        /* Clamp active selection to available cards on this page */
        if (page_apps > 0) {
            int cur_idx = active_row * 2 + active_col;
            if (cur_idx >= page_apps) {
                active_row = (page_apps - 1) / 2;
                active_col = (page_apps - 1) % 2;
                needs_redraw = true;
            }
        }

        atlas_input_state_t in = atlas_input_get_state();

        /* ── Joystick navigation (edge-detected) ─────────────────────────── */
        if (!joy_latched) {
            if (page_apps > 0) {
                if (in.joy_x > 50) {
                    if (active_col == 0 && (active_row * 2 + 1) < page_apps) {
                        active_col = 1;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(880.0f, 15);
                    } else if (current_page < total_pages - 1) {
                        /* Next page */
                        current_page++;
                        active_col = 0;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(980.0f, 20);
                    }
                } else if (in.joy_x < -50) {
                    if (active_col == 1) {
                        active_col = 0;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(880.0f, 15);
                    } else if (current_page > 0) {
                        /* Prev page */
                        current_page--;
                        active_col = 1;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(780.0f, 20);
                    }
                }

                if (in.joy_y > 50) {
                    if (active_row == 0 && (2 + active_col) < page_apps) {
                        active_row = 1;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(880.0f, 15);
                    }
                } else if (in.joy_y < -50) {
                    if (active_row == 1) {
                        active_row = 0;
                        joy_latched = true;
                        needs_redraw = true;
                        play_ui_beep(880.0f, 15);
                    }
                }
            }
        } else if (abs(in.joy_x) < 25 && abs(in.joy_y) < 25) {
            joy_latched = false;
        }

        int active_idx = page_start + (active_row * 2 + active_col);
        atlas_app_t active_app;
        bool has_active_app = (app_manager_get_at(active_idx, &active_app) == ESP_OK);

        /* ── Button A: Launch Application ───────────────────────────────── */
        if (in.btn_a && !last_btn_a) {
            if (has_active_app) {
                if (active_app.is_launchable) {
                    play_ui_beep(1200.0f, 40);
                    esp_err_t launch_err = app_manager_launch(active_app.app_id);
                    needs_redraw = true;

                    if (launch_err == ESP_OK) {
                        snprintf(toast_msg, sizeof(toast_msg), "RETURNED: %s", active_app.name);
                        toast_timer = 35;
                    } else {
                        snprintf(toast_msg, sizeof(toast_msg), "ERROR: %s", esp_err_to_name(launch_err));
                        toast_timer = 50;
                    }
                } else {
                    /* Honest notification of unsupported runtime */
                    play_ui_beep(300.0f, 50);
                    snprintf(toast_msg, sizeof(toast_msg), "UNSUPPORTED: Runtime '%s' unavailable",
                             active_app.runtime_str);
                    toast_timer = 50;
                    needs_redraw = true;
                    ESP_LOGW(TAG, "Cannot launch '%s': runtime '%s' is not supported.",
                             active_app.app_id, active_app.runtime_str);
                }
            } else {
                play_ui_beep(300.0f, 30);
                snprintf(toast_msg, sizeof(toast_msg), "NO APP SELECTED");
                toast_timer = 30;
                needs_redraw = true;
            }
        }

        /* ── Button B: Focus Reset / First Page ─────────────────────────── */
        if (in.btn_b && !last_btn_b) {
            current_page = 0;
            active_row   = 0;
            active_col   = 0;
            snprintf(toast_msg, sizeof(toast_msg), "PAGE 1");
            toast_timer = 25;
            needs_redraw = true;
            play_ui_beep(587.33f, 25);
        }

        /* ── Button C: Prev Page or Rescan ──────────────────────────────── */
        if (in.btn_c && !last_btn_c) {
            play_ui_beep(783.99f, 25);
            if (total_pages > 1) {
                if (current_page > 0) current_page--;
                else current_page = total_pages - 1;
                snprintf(toast_msg, sizeof(toast_msg), "PAGE %d / %d", current_page + 1, total_pages);
            } else {
                app_manager_scan_installed();
                snprintf(toast_msg, sizeof(toast_msg), "RESCANNED SD STORAGE");
            }
            toast_timer = 35;
            needs_redraw = true;
        }

        /* ── Button D: Next Page or Rescan ──────────────────────────────── */
        if (in.btn_d && !last_btn_d) {
            play_ui_beep(987.77f, 25);
            if (total_pages > 1) {
                if (current_page < total_pages - 1) current_page++;
                else current_page = 0;
                snprintf(toast_msg, sizeof(toast_msg), "PAGE %d / %d", current_page + 1, total_pages);
            } else {
                app_manager_scan_installed();
                snprintf(toast_msg, sizeof(toast_msg), "RESCANNED SD STORAGE");
            }
            toast_timer = 35;
            needs_redraw = true;
        }

        last_btn_a = in.btn_a;
        last_btn_b = in.btn_b;
        last_btn_c = in.btn_c;
        last_btn_d = in.btn_d;

        if (toast_timer > 0) {
            toast_timer--;
            needs_redraw = true;
        }

        /* ── Render Dynamic UI (only on state change) ───────────────────── */
        if (needs_redraw) {
            needs_redraw = false;

            atlas_gfx_clear(fb, COLOR_BG_DARK);

            /* Header */
            atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);

            char title_bar[64];
            if (total_pages > 1) {
                snprintf(title_bar, sizeof(title_bar), "ATLAS OS (Page %d/%d)", current_page + 1, total_pages);
            } else {
                snprintf(title_bar, sizeof(title_bar), "ATLAS OS // LAUNCHER");
            }
            atlas_gfx_draw_string(fb, 12, 8, title_bar, COLOR_CYAN, 0x0001, 1);

            /* SD Card Status Pill in Header */
            bool sd_ok = atlas_sdcard_is_mounted();
            if (sd_ok) {
                atlas_gfx_draw_string(fb, 230, 8, "[SD:OK]", COLOR_GREEN, 0x0001, 1);
            } else {
                atlas_gfx_draw_string(fb, 230, 8, "[NO SD]", COLOR_YELLOW, 0x0001, 1);
            }

            /* ── Card Grid or Empty State ───────────────────────────────── */
            if (total_apps == 0) {
                /* Empty catalogue state */
                atlas_gfx_fill_rect(fb, 20, 60, 280, 100, COLOR_PANEL_BG);
                atlas_gfx_draw_rect(fb, 20, 60, 280, 100, COLOR_BORDER_NORM);
                atlas_gfx_draw_string_centered(fb, 85,  "NO APPLICATIONS INSTALLED", COLOR_YELLOW, 0x0001, 1);
                atlas_gfx_draw_string_centered(fb, 105, "Install apps to /sdcard/apps/installed/", COLOR_TEXT_MUTED, 0x0001, 1);
                atlas_gfx_draw_string_centered(fb, 125, "[C] Rescan Storage", COLOR_CYAN, 0x0001, 1);
            } else {
                for (int r = 0; r < 2; r++) {
                    for (int c = 0; c < 2; c++) {
                        int slot = r * 2 + c;
                        int app_i = page_start + slot;
                        if (app_i >= (int)total_apps) continue;

                        atlas_app_t app;
                        if (app_manager_get_at(app_i, &app) != ESP_OK) continue;

                        bool is_active = (r == active_row && c == active_col);

                        char card_tagline[48];
                        if (app.is_launchable) {
                            snprintf(card_tagline, sizeof(card_tagline), "%s", app.tagline);
                        } else {
                            snprintf(card_tagline, sizeof(card_tagline), "[%s: unexecutable]", app.runtime_str);
                        }

                        atlas_gfx_draw_card(fb, COL_X[c], ROW_Y[r], CARD_W, CARD_H,
                                            app.name, card_tagline, is_active);

                        /* Icon or category indicator badge */
                        uint16_t badge_col = (app.type == ATLAS_APP_TYPE_BUILTIN) ? COLOR_CYAN : COLOR_PURPLE;
                        atlas_gfx_fill_rect(fb, COL_X[c] + CARD_W - 14, ROW_Y[r] + 8, 6, 6, badge_col);
                    }
                }
            }

            /* ── Footer ─────────────────────────────────────────────────── */
            atlas_gfx_fill_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 38, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 202, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);

            if (toast_timer > 0) {
                uint16_t toast_color = COLOR_GREEN;
                if (strncmp(toast_msg, "UNSUPPORTED", 11) == 0 ||
                    strncmp(toast_msg, "ERROR",        5) == 0 ||
                    strncmp(toast_msg, "NO APP",       6) == 0) {
                    toast_color = COLOR_YELLOW;
                }
                atlas_gfx_draw_string_centered(fb, 214, toast_msg, toast_color, 0x0001, 1);
            } else {
                if (total_pages > 1) {
                    atlas_gfx_draw_string(fb, 12, 210, "[C] Prev Page       [D] Next Page", COLOR_CYAN, 0x0001, 1);
                } else {
                    atlas_gfx_draw_string(fb, 12, 210, "[C/D] Rescan SD Storage", COLOR_CYAN, 0x0001, 1);
                }
                atlas_gfx_draw_string(fb, 12, 224, "[A] Launch App      [B] Reset Focus", COLOR_TEXT_MUTED, 0x0001, 1);
            }

            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();
        }

        vTaskDelay(pdMS_TO_TICKS(20)); /* 50 Hz poll */
    }

    /* Unreachable */
    free(fb);
}
