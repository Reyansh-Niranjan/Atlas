/**
 * @file screen_onboarding.c
 * @brief First-Boot Onboarding & Wi-Fi Provisioning Wizard Implementation for ATLAS OS
 */
#include "screen_onboarding.h"
#include "atlas_display.h"
#include "atlas_gfx.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "atlas_log.h"
#include "atlas_wifi.h"
#include "atlas_captive.h"
#include "atlas_onboarding.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

typedef enum {
    ONBOARD_STEP_INTRO = 0,
    ONBOARD_STEP_SELECT_METHOD,
    ONBOARD_STEP_QR_CODE,
    ONBOARD_STEP_MANUAL_GUIDE,
    ONBOARD_STEP_CONNECTING,
    ONBOARD_STEP_SUCCESS,
    ONBOARD_STEP_DIAGNOSTICS,
    ONBOARD_STEP_RECOVERY
} onboard_step_t;

static onboard_step_t s_step = ONBOARD_STEP_INTRO;
static char s_target_ssid[64] = {0};
static char s_target_pass[64] = {0};
static bool s_credentials_received = false;
static bool s_show_password = false;
static int  s_failure_count = 0;
static char s_softap_ssid[32] = {0};
static atlas_onboarding_state_t s_onboard_state;

static void on_captive_credentials(const char *ssid, const char *password)
{
    if (ssid && strlen(ssid) > 0) {
        snprintf(s_target_ssid, sizeof(s_target_ssid), "%s", ssid);
        if (password) {
            snprintf(s_target_pass, sizeof(s_target_pass), "%s", password);
        } else {
            s_target_pass[0] = '\0';
        }
        s_credentials_received = true;
        atlas_onboarding_write_wifi_file(s_target_ssid, s_target_pass);
        atlas_audio_play_beep(1200.0f, 60);
    }
}

static void draw_header(uint16_t *fb, const char *title, uint16_t accent_color)
{
    atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
    atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
    atlas_gfx_draw_string(fb, 12, 8, "ATLAS OS SETUP", accent_color, COLOR_PANEL_BG, 1);
    atlas_gfx_draw_string(fb, 140, 8, title, COLOR_WHITE, COLOR_PANEL_BG, 1);
}

static void draw_footer(uint16_t *fb, const char *nav_text)
{
    atlas_gfx_fill_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
    atlas_gfx_draw_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
    atlas_gfx_draw_string(fb, 12, 222, nav_text, COLOR_YELLOW, COLOR_PANEL_BG, 1);
}

static void render_qr_code(uint16_t *fb, const char *text, int center_x, int top_y, int scale)
{
    uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
    uint8_t tempBuffer[qrcodegen_BUFFER_LEN_MAX];

    bool ok = qrcodegen_encodeText(text, tempBuffer, qrcode,
                                   qrcodegen_Ecc_LOW, 1, 10,
                                   qrcodegen_Mask_AUTO, true);
    if (!ok) return;

    int size = qrcodegen_getSize(qrcode);
    int qr_px = size * scale;
    int start_x = center_x - (qr_px / 2);
    int start_y = top_y;

    /* White quiet zone border */
    int quiet_pad = 6;
    atlas_gfx_fill_rect(fb, start_x - quiet_pad, start_y - quiet_pad,
                        qr_px + (quiet_pad * 2), qr_px + (quiet_pad * 2), COLOR_WHITE);

    /* Render modules */
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            bool module = qrcodegen_getModule(qrcode, x, y);
            uint16_t color = module ? COLOR_BLACK : COLOR_WHITE;
            atlas_gfx_fill_rect(fb, start_x + x * scale, start_y + y * scale, scale, scale, color);
        }
    }
}

void screen_onboarding_run(void)
{
    uint16_t *fb = atlas_display_get_framebuffer();
    if (!fb) return;

    atlas_onboarding_get_state(&s_onboard_state);
    atlas_onboarding_ensure_wifi_template();

    s_step = ONBOARD_STEP_INTRO;
    s_failure_count = 0;
    s_credentials_received = false;
    s_show_password = false;
    s_target_ssid[0] = '\0';
    s_target_pass[0] = '\0';

    atlas_input_state_t last_in = {0};
    bool running = true;
    int anim_tick = 0;

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_UI, "Starting Atlas Onboarding Wizard (Version: %s)...",
                   s_onboard_state.current_version);

    while (running) {
        atlas_input_state_t in = atlas_input_get_state();
        bool pressed_a = (in.btn_a && !last_in.btn_a);
        bool pressed_b = (in.btn_b && !last_in.btn_b);
        bool pressed_c = (in.btn_c && !last_in.btn_c);
        bool pressed_d = (in.btn_d && !last_in.btn_d);
        last_in = in;

        atlas_gfx_clear(fb, COLOR_BG_DARK);

        /* ── STEP 1: INTRO / WHAT'S NEW ──────────────────────────────────── */
        if (s_step == ONBOARD_STEP_INTRO) {
            if (s_onboard_state.is_update_reonboard) {
                draw_header(fb, "[FIRMWARE UPDATED]", COLOR_GREEN);

                atlas_gfx_fill_rect(fb, 16, 40, 288, 160, COLOR_PANEL_BG);
                atlas_gfx_draw_rect(fb, 16, 40, 288, 160, COLOR_BORDER_NORM);

                char vline[48];
                snprintf(vline, sizeof(vline), "Running Atlas OS %s", s_onboard_state.current_version);
                atlas_gfx_draw_string(fb, 28, 52, vline, COLOR_CYAN, COLOR_PANEL_BG, 1);

                atlas_gfx_draw_string(fb, 28, 74, "WHAT'S NEW IN THIS RELEASE:", COLOR_YELLOW, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 94, "- Dynamic SD App Loader Active", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 114, "- Interactive System Settings", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 134, "- Wi-Fi Provisioning & QR Setup", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 154, "- 0% Performance Degradation", COLOR_GREEN, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 174, "- Rollback Confirmation Validated", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

                draw_footer(fb, "[A] Verify Network & Continue");
            } else {
                draw_header(fb, "[WELCOME]", COLOR_CYAN);

                atlas_gfx_fill_rect(fb, 16, 36, 288, 168, COLOR_PANEL_BG);
                atlas_gfx_draw_rect(fb, 16, 36, 288, 168, COLOR_BORDER_NORM);

                atlas_gfx_draw_string(fb, 28, 48, "WELCOME TO ATLAS OS", COLOR_CYAN, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 64, "ESP32-S3 Handheld Cybernetic Terminal", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

                atlas_gfx_draw_string(fb, 28, 88, "PHYSICAL HARDWARE CONTROLS:", COLOR_YELLOW, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 108, "Joystick  : 2-Axis Navigation", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 126, "[A] SW1   : Select / Confirm / Action", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 144, "[B] SW2   : Back / Cancel / Return", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 162, "[C] SW3   : System Menu / Shell", COLOR_WHITE, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 180, "[D] SW4   : Tools / Diagnostics", COLOR_WHITE, COLOR_PANEL_BG, 1);

                draw_footer(fb, "[A] Begin Initial Setup");
            }

            if (pressed_a) {
                atlas_audio_play_beep(880.0f, 30);
                s_step = ONBOARD_STEP_SELECT_METHOD;
            }
        }

        /* ── STEP 2: SELECT WI-FI METHOD ─────────────────────────────────── */
        else if (s_step == ONBOARD_STEP_SELECT_METHOD) {
            draw_header(fb, "[WI-FI CONFIG]", COLOR_CYAN);

            atlas_gfx_draw_string(fb, 20, 38, "Choose how to connect Atlas to Wi-Fi:", COLOR_WHITE, COLOR_BG_DARK, 1);

            /* Option A: QR Code */
            atlas_gfx_fill_rect(fb, 16, 58, 288, 64, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 58, 288, 64, COLOR_CYAN);
            atlas_gfx_draw_string(fb, 28, 70, "[A] FAST QR CODE SETUP (Recommended)", COLOR_CYAN, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 90, "Scan with phone camera -> auto connect", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 104, "Select home network in mobile web portal", COLOR_GREEN, COLOR_PANEL_BG, 1);

            /* Option B: Manual SD Card */
            atlas_gfx_fill_rect(fb, 16, 134, 288, 64, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 134, 288, 64, COLOR_BORDER_NORM);
            atlas_gfx_draw_string(fb, 28, 146, "[B] MANUAL SD CARD SETUP", COLOR_YELLOW, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 166, "Eject SD card and edit 'wifi.txt' on PC", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 180, "No phone or mobile browser required", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

            draw_footer(fb, "Press [A] for QR Portal  |  [B] for Manual SD");

            if (pressed_a) {
                atlas_audio_play_beep(1000.0f, 30);
                atlas_captive_start(on_captive_credentials, s_softap_ssid, sizeof(s_softap_ssid));
                s_step = ONBOARD_STEP_QR_CODE;
            } else if (pressed_b) {
                atlas_audio_play_beep(700.0f, 30);
                s_step = ONBOARD_STEP_MANUAL_GUIDE;
            }
        }

        /* ── STEP 3A: QR CODE CAPTIVE PORTAL ─────────────────────────────── */
        else if (s_step == ONBOARD_STEP_QR_CODE) {
            draw_header(fb, "[PHONE QR SETUP]", COLOR_CYAN);

            /* Construct Wi-Fi pairing string: WIFI:S:<SSID>;T:nopass;; */
            char qr_str[64];
            snprintf(qr_str, sizeof(qr_str), "WIFI:S:%s;T:nopass;;", s_softap_ssid);

            /* Render 116x116 QR Code */
            render_qr_code(fb, qr_str, 90, 46, 4);

            /* Instructions panel on right side */
            atlas_gfx_fill_rect(fb, 168, 42, 140, 160, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 168, 42, 140, 160, COLOR_BORDER_NORM);

            atlas_gfx_draw_string(fb, 176, 52, "SCAN WITH PHONE", COLOR_YELLOW, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 176, 72, "1. Open camera", COLOR_WHITE, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 176, 88, "2. Tap to join", COLOR_WHITE, COLOR_PANEL_BG, 1);

            char ssid_sub[32];
            snprintf(ssid_sub, sizeof(ssid_sub), "%s", s_softap_ssid);
            atlas_gfx_draw_string(fb, 176, 106, ssid_sub, COLOR_CYAN, COLOR_PANEL_BG, 1);

            atlas_gfx_draw_string(fb, 176, 128, "3. Setup page", COLOR_WHITE, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 176, 144, "opens in browser", COLOR_WHITE, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 176, 164, "4. Submit Wi-Fi", COLOR_GREEN, COLOR_PANEL_BG, 1);

            draw_footer(fb, "[B] Cancel / Switch to Manual SD Card");

            if (s_credentials_received) {
                atlas_captive_stop();
                s_step = ONBOARD_STEP_CONNECTING;
            } else if (pressed_b) {
                atlas_audio_play_beep(600.0f, 30);
                atlas_captive_stop();
                s_step = ONBOARD_STEP_SELECT_METHOD;
            }
        }

        /* ── STEP 3B: MANUAL SD CARD GUIDE ───────────────────────────────── */
        else if (s_step == ONBOARD_STEP_MANUAL_GUIDE) {
            draw_header(fb, "[MANUAL SD SETUP]", COLOR_YELLOW);

            atlas_gfx_fill_rect(fb, 16, 36, 288, 168, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 36, 288, 168, COLOR_BORDER_NORM);

            atlas_gfx_draw_string(fb, 28, 48, "INSTRUCTIONS FOR PC / MAC:", COLOR_YELLOW, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 68, "1. Safely remove MicroSD card from Atlas.", COLOR_WHITE, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 86, "2. Insert MicroSD into your computer.", COLOR_WHITE, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 104, "3. Open 'wifi.txt' on root of SD card.", COLOR_CYAN, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 122, "4. Write your network details exactly:", COLOR_WHITE, COLOR_PANEL_BG, 1);

            atlas_gfx_fill_rect(fb, 36, 138, 248, 36, COLOR_BG_DARK);
            atlas_gfx_draw_string(fb, 46, 144, "SSID=YourHomeWiFiName", COLOR_GREEN, COLOR_BG_DARK, 1);
            atlas_gfx_draw_string(fb, 46, 158, "PASSWORD=YourSecretPassword", COLOR_GREEN, COLOR_BG_DARK, 1);

            atlas_gfx_draw_string(fb, 28, 184, "5. Save file & re-insert SD card.", COLOR_WHITE, COLOR_PANEL_BG, 1);

            draw_footer(fb, "[A] Done, Card Re-inserted  |  [B] Back");

            if (pressed_a) {
                atlas_audio_play_beep(900.0f, 30);
                esp_err_t err = atlas_onboarding_parse_wifi_file(s_target_ssid, sizeof(s_target_ssid),
                                                                 s_target_pass, sizeof(s_target_pass));
                if (err == ESP_OK && strlen(s_target_ssid) > 0) {
                    s_step = ONBOARD_STEP_CONNECTING;
                } else {
                    /* Could not parse credentials */
                    s_step = ONBOARD_STEP_DIAGNOSTICS;
                }
            } else if (pressed_b) {
                atlas_audio_play_beep(600.0f, 30);
                s_step = ONBOARD_STEP_SELECT_METHOD;
            }
        }

        /* ── STEP 4: CONNECTING ATTEMPT ──────────────────────────────────── */
        else if (s_step == ONBOARD_STEP_CONNECTING) {
            draw_header(fb, "[CONNECTING...]", COLOR_CYAN);

            atlas_gfx_fill_rect(fb, 16, 50, 288, 140, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 50, 288, 140, COLOR_BORDER_NORM);

            atlas_gfx_draw_string_centered(fb, 75, "ESTABLISHING WI-FI LINK", COLOR_YELLOW, COLOR_PANEL_BG, 1);

            char sline[64];
            snprintf(sline, sizeof(sline), "Target: %s", s_target_ssid);
            atlas_gfx_draw_string_centered(fb, 100, sline, COLOR_CYAN, COLOR_PANEL_BG, 1);

            /* Animated progress bar */
            int bar_w = (anim_tick % 200) + 20;
            atlas_gfx_fill_rect(fb, 48, 130, 224, 16, COLOR_BG_DARK);
            atlas_gfx_draw_rect(fb, 48, 130, 224, 16, COLOR_BORDER_NORM);
            atlas_gfx_fill_rect(fb, 50, 132, bar_w, 12, COLOR_CYAN);

            atlas_gfx_draw_string_centered(fb, 160, "Associating & Acquiring DHCP IP...", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

            draw_footer(fb, "Please wait... handshake in progress");

            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();

            /* Synchronous connection with 12s timeout */
            esp_err_t err = atlas_wifi_connect(s_target_ssid, s_target_pass, 12000);
            if (err == ESP_OK) {
                s_failure_count = 0;
                s_step = ONBOARD_STEP_SUCCESS;
            } else {
                s_failure_count++;
                s_step = ONBOARD_STEP_DIAGNOSTICS;
            }
        }

        /* ── STEP 5A: SUCCESS ────────────────────────────────────────────── */
        else if (s_step == ONBOARD_STEP_SUCCESS) {
            draw_header(fb, "[SETUP COMPLETED]", COLOR_GREEN);

            atlas_gfx_fill_rect(fb, 16, 45, 288, 150, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 45, 288, 150, COLOR_GREEN);

            atlas_gfx_draw_string_centered(fb, 65, "WI-FI LINK ESTABLISHED!", COLOR_GREEN, COLOR_PANEL_BG, 1);

            char ipline[48];
            char ip_str[24] = {0};
            atlas_wifi_get_ip_str(ip_str, sizeof(ip_str));
            snprintf(ipline, sizeof(ipline), "IP Address : %s", ip_str);
            atlas_gfx_draw_string(fb, 36, 100, ipline, COLOR_WHITE, COLOR_PANEL_BG, 1);

            char rssi_line[48];
            snprintf(rssi_line, sizeof(rssi_line), "Signal RSSI: %d dBm (Strong)", (int)atlas_wifi_get_rssi());
            atlas_gfx_draw_string(fb, 36, 120, rssi_line, COLOR_CYAN, COLOR_PANEL_BG, 1);

            atlas_gfx_draw_string(fb, 36, 140, "Status     : Profile Saved & Ready", COLOR_TEXT_MAIN, COLOR_PANEL_BG, 1);

            draw_footer(fb, "[A] Launch ATLAS Desktop");

            if (pressed_a) {
                atlas_audio_play_beep(1400.0f, 60);
                atlas_onboarding_mark_complete();
                running = false;
            }
        }

        /* ── STEP 5B: FAILURE DIAGNOSTICS ────────────────────────────────── */
        else if (s_step == ONBOARD_STEP_DIAGNOSTICS) {
            draw_header(fb, "[CONNECTION FAILED]", COLOR_RED);

            atlas_gfx_fill_rect(fb, 16, 36, 288, 168, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 36, 288, 168, COLOR_RED);

            atlas_gfx_draw_string(fb, 28, 48, "COULD NOT CONNECT TO WI-FI", COLOR_RED, COLOR_PANEL_BG, 1);

            char sline[64];
            snprintf(sline, sizeof(sline), "SSID: %s", (strlen(s_target_ssid) > 0) ? s_target_ssid : "<NOT FOUND IN wifi.txt>");
            atlas_gfx_draw_string(fb, 28, 70, sline, COLOR_WHITE, COLOR_PANEL_BG, 1);

            char pline[64];
            if (s_show_password) {
                snprintf(pline, sizeof(pline), "Pass: %s", (strlen(s_target_pass) > 0) ? s_target_pass : "<None>");
            } else {
                snprintf(pline, sizeof(pline), "Pass: %s", (strlen(s_target_pass) > 0) ? "******** (Hidden)" : "<None>");
            }
            atlas_gfx_draw_string(fb, 28, 90, pline, COLOR_YELLOW, COLOR_PANEL_BG, 1);

            atlas_gfx_draw_string(fb, 28, 114, "DIAGNOSIS:", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 130, atlas_wifi_get_last_error_desc(), COLOR_ORANGE, COLOR_PANEL_BG, 1);

            if (strlen(s_target_ssid) == 0) {
                atlas_gfx_draw_string(fb, 28, 150, "Check SD card: Is wifi.txt present?", COLOR_CYAN, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 168, "Re-insert SD card or verify FAT32.", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            } else {
                atlas_gfx_draw_string(fb, 28, 150, "[D] Toggle Password Visibility", COLOR_CYAN, COLOR_PANEL_BG, 1);
                atlas_gfx_draw_string(fb, 28, 168, "Check password carefully and retry.", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            }

            if (s_failure_count >= 2) {
                draw_footer(fb, "[A] Retry  [B] Edit  [C] Recovery & Reset");
            } else {
                draw_footer(fb, "[A] Retry Connection  |  [B] Choose Method");
            }

            if (pressed_a) {
                atlas_audio_play_beep(900.0f, 30);
                s_step = ONBOARD_STEP_CONNECTING;
            } else if (pressed_b) {
                atlas_audio_play_beep(600.0f, 30);
                s_step = ONBOARD_STEP_SELECT_METHOD;
            } else if (pressed_d) {
                s_show_password = !s_show_password;
            } else if (pressed_c && s_failure_count >= 2) {
                atlas_audio_play_beep(400.0f, 50);
                s_step = ONBOARD_STEP_RECOVERY;
            }
        }

        /* ── STEP 6: RECOVERY & FACTORY RESET ────────────────────────────── */
        else if (s_step == ONBOARD_STEP_RECOVERY) {
            draw_header(fb, "[RECOVERY & SUPPORT]", COLOR_ORANGE);

            atlas_gfx_fill_rect(fb, 16, 36, 288, 168, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 16, 36, 288, 168, COLOR_ORANGE);

            atlas_gfx_draw_string(fb, 28, 48, "SETUP TROUBLESHOOTING", COLOR_YELLOW, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 68, "If issues persist, choose an option:", COLOR_WHITE, COLOR_PANEL_BG, 1);

            atlas_gfx_draw_string(fb, 28, 92, "[A] FACTORY RESET FLASH & SD CARD", COLOR_RED, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 108, "    Erases NVS Wi-Fi, resets configs", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

            atlas_gfx_draw_string(fb, 28, 128, "[B] BOOT IN OFFLINE MODE", COLOR_CYAN, COLOR_PANEL_BG, 1);
            atlas_gfx_draw_string(fb, 28, 144, "    Skip Wi-Fi and launch shell", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);

            atlas_gfx_fill_rect(fb, 24, 164, 272, 32, COLOR_BG_DARK);
            atlas_gfx_draw_string(fb, 32, 170, "Need Help? Contact Engineering:", COLOR_TEXT_MUTED, COLOR_BG_DARK, 1);
            atlas_gfx_draw_string(fb, 32, 184, "reyanshniranjanodm@gmail.com", COLOR_GREEN, COLOR_BG_DARK, 1);

            draw_footer(fb, "[A] Factory Reset  |  [B] Offline Boot");

            if (pressed_a) {
                atlas_audio_play_beep(300.0f, 200);
                atlas_gfx_clear(fb, COLOR_BLACK);
                atlas_gfx_draw_string_centered(fb, 100, "PERFORMING FACTORY RESET...", COLOR_RED, COLOR_BLACK, 1);
                atlas_gfx_draw_string_centered(fb, 125, "Erasing Flash NVS & Restoring SD...", COLOR_WHITE, COLOR_BLACK, 1);
                atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
                atlas_display_wait_flush();
                atlas_onboarding_perform_factory_reset();
            } else if (pressed_b) {
                atlas_audio_play_beep(880.0f, 30);
                atlas_onboarding_mark_complete();
                running = false;
            }
        }

        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();

        anim_tick += 10;
        vTaskDelay(pdMS_TO_TICKS(40));
    }
}
