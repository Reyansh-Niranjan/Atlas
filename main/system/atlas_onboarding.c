/**
 * @file atlas_onboarding.c
 * @brief Onboarding Lifecycle & Configuration Implementation for ATLAS OS
 */
#include "atlas_onboarding.h"
#include "atlas_log.h"
#include "atlas_sdcard.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_app_desc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NVS_NAMESPACE_CFG        "atlas_cfg"
#define NVS_KEY_ONBOARD_VER      "onboard_ver"

static void trim_whitespace(char *str)
{
    if (!str) return;
    char *end;

    /* Trim leading space */
    char *start = str;
    while (isspace((unsigned char)*start)) start++;
    if (*start == 0) {
        *str = '\0';
        return;
    }

    /* Trim trailing space */
    end = start + strlen(start) - 1;
    while (end > start && (isspace((unsigned char)*end) || *end == '\r' || *end == '\n')) end--;
    end[1] = '\0';

    if (start != str) {
        memmove(str, start, strlen(start) + 1);
    }
}

static void strip_surrounding_quotes(char *str)
{
    if (!str) return;
    size_t len = strlen(str);
    if (len >= 2 && ((str[0] == '"' && str[len - 1] == '"') ||
                     (str[0] == '\'' && str[len - 1] == '\''))) {
        str[len - 1] = '\0';
        memmove(str, str + 1, len - 1);
    }
}

esp_err_t atlas_onboarding_get_state(atlas_onboarding_state_t *out_state)
{
    if (!out_state) return ESP_ERR_INVALID_ARG;
    memset(out_state, 0, sizeof(atlas_onboarding_state_t));

    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_ver = app_desc ? app_desc->version : "1.0.0";
    snprintf(out_state->current_version, sizeof(out_state->current_version), "%s", current_ver);

    /* 1. Try reading from SD card */
    FILE *f = fopen(ATLAS_ONBOARDING_FILE_PATH, "r");
    if (f) {
        char buf[512] = {0};
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char *completed_str = strstr(buf, "\"onboarding_completed\"");
        if (completed_str) {
            const char *true_ptr = strstr(completed_str, "true");
            const char *false_ptr = strstr(completed_str, "false");
            if (true_ptr && (!false_ptr || true_ptr < false_ptr)) {
                out_state->is_completed = true;
            }
        }

        char *ver_str = strstr(buf, "\"completed_os_version\"");
        if (ver_str) {
            char *colon = strchr(ver_str, ':');
            if (colon) {
                char *quote1 = strchr(colon, '"');
                if (quote1) {
                    const char *quote2 = strchr(quote1 + 1, '"');
                    if (quote2) {
                        size_t vlen = quote2 - (quote1 + 1);
                        if (vlen >= sizeof(out_state->completed_version)) {
                            vlen = sizeof(out_state->completed_version) - 1;
                        }
                        strncpy(out_state->completed_version, quote1 + 1, vlen);
                        out_state->completed_version[vlen] = '\0';
                    }
                }
            }
        }

        if (out_state->is_completed) {
            /* Check if version changed (OTA firmware update) */
            if (strlen(out_state->completed_version) > 0 &&
                strcmp(out_state->completed_version, current_ver) != 0) {
                out_state->is_completed = false;
                out_state->is_update_reonboard = true;
                ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "OS updated from %s to %s. Re-onboarding required.",
                               out_state->completed_version, current_ver);
            }
        }
        return ESP_OK;
    }

    /* 2. Fallback: check NVS if SD card file is absent */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READONLY, &nvs) == ESP_OK) {
        size_t vlen = sizeof(out_state->completed_version);
        if (nvs_get_str(nvs, NVS_KEY_ONBOARD_VER, out_state->completed_version, &vlen) == ESP_OK) {
            if (strcmp(out_state->completed_version, current_ver) == 0) {
                out_state->is_completed = true;
            } else {
                out_state->is_completed = false;
                out_state->is_update_reonboard = true;
            }
        }
        nvs_close(nvs);
    }

    return ESP_OK;
}

bool atlas_onboarding_is_required(void)
{
    atlas_onboarding_state_t st;
    atlas_onboarding_get_state(&st);
    return !st.is_completed;
}

esp_err_t atlas_onboarding_mark_complete(void)
{
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const char *current_ver = app_desc ? app_desc->version : "1.0.0";
    const char *build_date = app_desc ? app_desc->date : "2026-11-04";

    /* Ensure /sdcard/config exists */
    mkdir("/sdcard/config", 0777);

    FILE *f = fopen(ATLAS_ONBOARDING_FILE_PATH, "w");
    if (f) {
        fprintf(f, "{\n");
        fprintf(f, "  \"version\": 1,\n");
        fprintf(f, "  \"onboarding_completed\": true,\n");
        fprintf(f, "  \"completed_os_version\": \"%s\",\n", current_ver);
        fprintf(f, "  \"completed_build_date\": \"%s\",\n", build_date);
        fprintf(f, "  \"wifi_configured\": true\n");
        fprintf(f, "}\n");
        fclose(f);
    }

    /* Save to NVS */
    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_set_str(nvs, NVS_KEY_ONBOARD_VER, current_ver);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Onboarding marked complete for version %s", current_ver);
    return ESP_OK;
}

esp_err_t atlas_onboarding_invalidate(void)
{
    FILE *f = fopen(ATLAS_ONBOARDING_FILE_PATH, "w");
    if (f) {
        fprintf(f, "{\n  \"onboarding_completed\": false\n}\n");
        fclose(f);
    }

    nvs_handle_t nvs;
    if (nvs_open(NVS_NAMESPACE_CFG, NVS_READWRITE, &nvs) == ESP_OK) {
        nvs_erase_key(nvs, NVS_KEY_ONBOARD_VER);
        nvs_commit(nvs);
        nvs_close(nvs);
    }

    return ESP_OK;
}

esp_err_t atlas_onboarding_parse_wifi_file(char *out_ssid, size_t ssid_len,
                                          char *out_pass, size_t pass_len)
{
    if (!out_ssid || ssid_len == 0) return ESP_ERR_INVALID_ARG;
    out_ssid[0] = '\0';
    if (out_pass && pass_len > 0) out_pass[0] = '\0';

    FILE *f = fopen(ATLAS_WIFI_TXT_PATH, "r");
    if (!f) {
        /* Check fallback /sdcard/config/wifi.txt */
        f = fopen("/sdcard/config/wifi.txt", "r");
    }
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[128];
    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);
        strip_surrounding_quotes(val);

        if (strcasecmp(key, "SSID") == 0) {
            snprintf(out_ssid, ssid_len, "%s", val);
        } else if (strcasecmp(key, "PASSWORD") == 0 || strcasecmp(key, "PASS") == 0) {
            if (out_pass && pass_len > 0) {
                snprintf(out_pass, pass_len, "%s", val);
            }
        }
    }
    fclose(f);

    return (out_ssid[0] != '\0') ? ESP_OK : ESP_FAIL;
}

esp_err_t atlas_onboarding_ensure_wifi_template(void)
{
    struct stat st;
    if (stat(ATLAS_WIFI_TXT_PATH, &st) == 0 && st.st_size > 0) {
        return ESP_OK;
    }

    FILE *f = fopen(ATLAS_WIFI_TXT_PATH, "w");
    if (!f) return ESP_FAIL;

    fprintf(f, "# =========================================================\n");
    fprintf(f, "# ATLAS HANDHELD TERMINAL - WI-FI CONFIGURATION\n");
    fprintf(f, "# =========================================================\n");
    fprintf(f, "# Enter your Wi-Fi credentials below, save the file,\n");
    fprintf(f, "# and re-insert the MicroSD card into your Atlas device.\n\n");
    fprintf(f, "SSID=\n");
    fprintf(f, "PASSWORD=\n");
    fclose(f);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Created template wifi.txt on SD card");
    return ESP_OK;
}

esp_err_t atlas_onboarding_write_wifi_file(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(ATLAS_WIFI_TXT_PATH, "w");
    if (!f) return ESP_FAIL;

    fprintf(f, "# ATLAS HANDHELD TERMINAL - WI-FI CONFIGURATION\n");
    fprintf(f, "SSID=%s\n", ssid);
    fprintf(f, "PASSWORD=%s\n", password ? password : "");
    fclose(f);

    return ESP_OK;
}

esp_err_t atlas_onboarding_parse_gemini_key(char *out_key, size_t key_len)
{
    if (!out_key || key_len == 0) return ESP_ERR_INVALID_ARG;
    out_key[0] = '\0';

    FILE *f = fopen(ATLAS_GEMINI_KEY_TXT_PATH, "r");
    if (!f) {
        f = fopen("/sdcard/config/gemini_key.txt", "r");
    }
    if (!f) return ESP_ERR_NOT_FOUND;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        trim_whitespace(line);
        if (line[0] == '\0' || line[0] == '#' || line[0] == ';') {
            continue;
        }

        char *eq = strchr(line, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_whitespace(key);
        trim_whitespace(val);
        strip_surrounding_quotes(val);

        if (strcasecmp(key, "GEMINI_API_KEY") == 0 || strcasecmp(key, "API_KEY") == 0) {
            snprintf(out_key, key_len, "%s", val);
            break;
        }
    }
    fclose(f);

    return (out_key[0] != '\0') ? ESP_OK : ESP_FAIL;
}

esp_err_t atlas_onboarding_write_gemini_key(const char *api_key)
{
    if (!api_key || strlen(api_key) == 0) return ESP_ERR_INVALID_ARG;

    FILE *f = fopen(ATLAS_GEMINI_KEY_TXT_PATH, "w");
    if (!f) return ESP_FAIL;

    fprintf(f, "# =========================================================\n");
    fprintf(f, "# ATLAS OS - GEMINI LIVE AI COMMUNICATOR KEY\n");
    fprintf(f, "# =========================================================\n");
    fprintf(f, "GEMINI_API_KEY=%s\n", api_key);
    fclose(f);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "Saved Gemini API key to %s", ATLAS_GEMINI_KEY_TXT_PATH);
    return ESP_OK;
}

/* ── Bundled System Assets & Auto-Seeding Tables ──────────────────────────── */

static const char s_readme_txt[] =
    "=========================================================\n"
    "ATLAS OS - MicroSD Storage Directory Guide\n"
    "=========================================================\n"
    "Welcome to ATLAS OS for ESP32-S3!\n\n"
    "Directory Structure:\n"
    "  /apps/installed/     - Dynamically loaded application bundles\n"
    "  /packages/downloads/ - Downloaded .apkg update archives\n"
    "  /system/firmware/    - OTA update binaries (.bin)\n"
    "  /system/recovery/    - Emergency recovery payloads\n"
    "  /data/               - Application save states and persistent files\n"
    "  /config/             - System and network configuration\n"
    "  /cache/              - Temporary runtime cache\n"
    "  /logs/               - System diagnostic logs\n\n"
    "Configuration Files:\n"
    "  /wifi.txt            - Quick Wi-Fi setup (SSID=..., PASSWORD=...)\n"
    "  /gemini_key.txt      - Google Gemini 3.8 API Key (GEMINI_API_KEY=...)\n\n"
    "Support & Feedback:\n"
    "  Email: reyanshniranjanodm@gmail.com\n"
    "=========================================================\n";

static const char s_gemini_key_template[] =
    "# =========================================================\n"
    "# ATLAS OS - GEMINI LIVE AI COMMUNICATOR KEY\n"
    "# =========================================================\n"
    "# Enter your Google Gemini API key below:\n"
    "# GEMINI_API_KEY=AIzaSy...\n"
    "# Get a key at: https://aistudio.google.com/app/apikey\n\n"
    "GEMINI_API_KEY=\n";

typedef struct {
    const char    *app_id;
    const char    *manifest_json;
    const uint8_t *bin_bytes;
    size_t         bin_len;
} atlas_bundled_app_t;

static const char s_manifest_assistant[] =
    "{\n"
    "  \"app_id\": \"org.atlas.assistant\",\n"
    "  \"name\": \"Gemini Live\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"Gemini 3.8 Extended Thinking AI\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"Real-time voice and reasoning AI communicator powered by Google Gemini 3.8 Flash. Features dual-pane thinking inspector and matrix answer terminal with push-to-talk voice capture.\",\n"
    "  \"author\": \"Atlas AI Team\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"audio_tx\",\n"
    "    \"audio_rx\",\n"
    "    \"network\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_calc[] =
    "{\n"
    "  \"app_id\": \"org.atlas.calc\",\n"
    "  \"name\": \"Scientific Calc\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"Math & Trig Engine\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"High-precision scientific calculator with trigonometry, logarithms, exponential functions, and floating-point math.\",\n"
    "  \"author\": \"Atlas Core Team\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_invaders[] =
    "{\n"
    "  \"app_id\": \"org.atlas.invaders\",\n"
    "  \"name\": \"Space Invaders\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"Retro Arcade Game\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"Classic 60 FPS arcade space shooter with joystick control, laser firing, alien collision detection, and retro audio sound effects.\",\n"
    "  \"author\": \"Retro Lab\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"audio_tx\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_notes[] =
    "{\n"
    "  \"app_id\": \"org.atlas.notes\",\n"
    "  \"name\": \"QuickNotes\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"Text Editor & Scratchpad\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"Lightweight notepad and system scratchpad for quick memos and hardware notes.\",\n"
    "  \"author\": \"Atlas Core Team\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"storage_app\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_settings[] =
    "{\n"
    "  \"app_id\": \"org.atlas.settings\",\n"
    "  \"name\": \"Settings\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"System & Telemetry Settings\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"System configuration and hardware telemetry app. Manages master audio volume, UI sound FX, display themes, SD storage, and OTA diagnostics.\",\n"
    "  \"author\": \"Atlas Core Team\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"audio_tx\",\n"
    "    \"storage_app\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_store[] =
    "{\n"
    "  \"app_id\": \"org.atlas.store\",\n"
    "  \"name\": \"APKG Store\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"Install & Manage Packages\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"On-device package manager and store for discovering, installing, and managing native applications on ATLAS OS.\",\n"
    "  \"author\": \"Atlas Core Team\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"audio_tx\",\n"
    "    \"storage_app\",\n"
    "    \"storage_shared\"\n"
    "  ]\n"
    "}\n";

static const char s_manifest_synth[] =
    "{\n"
    "  \"app_id\": \"org.atlas.synth\",\n"
    "  \"name\": \"Chiptune Synth\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"tagline\": \"I2S Wave Synthesizer\",\n"
    "  \"runtime\": \"native\",\n"
    "  \"entrypoint\": \"app.bin\",\n"
    "  \"description\": \"Live 8-bit sound synthesizer using MAX98357A I2S DAC with square/sawtooth waveforms and real-time frequency modulation.\",\n"
    "  \"author\": \"Atlas Audio Lab\",\n"
    "  \"license\": \"MIT\",\n"
    "  \"permissions\": [\n"
    "    \"display\",\n"
    "    \"input\",\n"
    "    \"audio_tx\"\n"
    "  ]\n"
    "}\n";

static const uint8_t s_app_bin_gemini[] = "ATLAS_APP_GEMINI_LIVE_V1\n\n";
static const uint8_t s_app_bin_calc[] = {
    'A', 'T', 'L', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA, 0x0A
};
static const uint8_t s_app_bin_invaders[] = {
    'A', 'T', 'L', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA, 0x0A
};
static const uint8_t s_app_bin_notes[] = {
    'A', 'T', 'L', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA, 0x0A
};
static const uint8_t s_app_bin_settings[] = {
    'A', 'T', 'L', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA, 0x0A
};
static const uint8_t s_app_bin_store[] = {
    'A', 'T', 'L', 'S', 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA
};
static const uint8_t s_app_bin_synth[] = {
    'A', 'T', 'L', 'S', ' ', ' ', ' ', ' ', ' ', ' ', ' ', 0x55, 0xC2, 0xAA, 0x55, 0xC2, 0xAA, 0x0A
};

static const atlas_bundled_app_t s_bundled_apps[] = {
    { "org.atlas.store",     s_manifest_store,     s_app_bin_store,     sizeof(s_app_bin_store) },
    { "org.atlas.settings",  s_manifest_settings,  s_app_bin_settings,  sizeof(s_app_bin_settings) },
    { "org.atlas.calc",      s_manifest_calc,      s_app_bin_calc,      sizeof(s_app_bin_calc) },
    { "org.atlas.notes",     s_manifest_notes,     s_app_bin_notes,     sizeof(s_app_bin_notes) },
    { "org.atlas.synth",     s_manifest_synth,     s_app_bin_synth,     sizeof(s_app_bin_synth) },
    { "org.atlas.invaders",  s_manifest_invaders,  s_app_bin_invaders,  sizeof(s_app_bin_invaders) },
    { "org.atlas.assistant", s_manifest_assistant, s_app_bin_gemini,    sizeof(s_app_bin_gemini) - 1 },
};

static bool file_exists_on_sd(const char *path)
{
    struct stat st;
    return (stat(path, &st) == 0 && st.st_size > 0);
}

static esp_err_t write_file_if_missing(const char *path, const void *data, size_t len)
{
    if (file_exists_on_sd(path)) {
        return ESP_OK;
    }
    FILE *f = fopen(path, "wb");
    if (!f) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "Failed to create missing file: %s", path);
        return ESP_FAIL;
    }
    size_t written = fwrite(data, 1, len, f);
    fclose(f);
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t atlas_onboarding_populate_sdcard(void)
{
    if (!atlas_sdcard_is_mounted()) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "SD card not mounted, cannot populate required files.");
        return ESP_ERR_INVALID_STATE;
    }

    size_t created_count = 0;

    /* 1. Ensure Standard Directory Structure */
    static const char *s_std_dirs[] = {
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
    for (size_t i = 0; i < sizeof(s_std_dirs) / sizeof(s_std_dirs[0]); i++) {
        struct stat st;
        if (stat(s_std_dirs[i], &st) != 0) {
            if (mkdir(s_std_dirs[i], 0777) == 0) {
                created_count++;
            }
        }
    }

    /* 2. Root Documentation Guide */
    if (!file_exists_on_sd("/sdcard/README.txt")) {
        if (write_file_if_missing("/sdcard/README.txt", s_readme_txt, strlen(s_readme_txt)) == ESP_OK) {
            created_count++;
        }
    }

    /* 3. Wi-Fi Configuration Template */
    if (!file_exists_on_sd(ATLAS_WIFI_TXT_PATH)) {
        if (atlas_onboarding_ensure_wifi_template() == ESP_OK) {
            created_count++;
        }
    }

    /* 4. Gemini API Key Template */
    if (!file_exists_on_sd(ATLAS_GEMINI_KEY_TXT_PATH)) {
        if (write_file_if_missing(ATLAS_GEMINI_KEY_TXT_PATH, s_gemini_key_template, strlen(s_gemini_key_template)) == ESP_OK) {
            created_count++;
        }
    }

    /* 5. Pre-seed Default Application Packages in /sdcard/apps/installed/<app_id>/ */
    for (size_t i = 0; i < sizeof(s_bundled_apps) / sizeof(s_bundled_apps[0]); i++) {
        const atlas_bundled_app_t *app = &s_bundled_apps[i];

        char app_dir[96];
        snprintf(app_dir, sizeof(app_dir), "/sdcard/apps/installed/%s", app->app_id);
        struct stat st_dir;
        if (stat(app_dir, &st_dir) != 0) {
            if (mkdir(app_dir, 0777) == 0) {
                created_count++;
            }
        }

        char manifest_path[128];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", app_dir);
        if (!file_exists_on_sd(manifest_path)) {
            if (write_file_if_missing(manifest_path, app->manifest_json, strlen(app->manifest_json)) == ESP_OK) {
                created_count++;
            }
        }

        char bin_path[128];
        snprintf(bin_path, sizeof(bin_path), "%s/app.bin", app_dir);
        if (!file_exists_on_sd(bin_path)) {
            if (write_file_if_missing(bin_path, app->bin_bytes, app->bin_len) == ESP_OK) {
                created_count++;
            }
        }
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_SYS, "SD card required structure verified (%u items created).",
                   (unsigned int)created_count);
    return ESP_OK;
}

void atlas_onboarding_perform_factory_reset(void)
{
    ATLAS_LOG_WARN(ATLAS_LOG_TAG_SYS, "Performing complete Flash & SD Factory Reset...");

    /* 1. Erase NVS flash */
    nvs_flash_erase();
    nvs_flash_init();

    /* 2. Unlink SD card configs */
    unlink(ATLAS_ONBOARDING_FILE_PATH);
    unlink(ATLAS_WIFI_TXT_PATH);
    unlink("/sdcard/config/wifi.txt");

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}
