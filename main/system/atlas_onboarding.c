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
