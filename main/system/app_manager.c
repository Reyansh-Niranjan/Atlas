/**
 * @file app_manager.c
 * @brief Dynamic Application Manager Implementation for ATLAS OS
 */
#include "app_manager.h"
#include "atlas_log.h"
#include "atlas_event.h"
#include "atlas_sdcard.h"
#include "atlas_ota.h"
#include "screen_settings.h"
#include "atlas_display.h"
#include "atlas_gfx.h"
#include "atlas_input.h"
#include "atlas_audio.h"
#include "wasm3.h"
#include "m3_env.h"
#include "esp_heap_caps.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define MANIFEST_MAX_FILE_SIZE  2048
#define APPS_INSTALLED_DIR      "/sdcard/apps/installed"

static atlas_app_t       s_apps[ATLAS_APP_MAX_COUNT];
static size_t            s_app_count = 0;
static SemaphoreHandle_t s_app_mutex = NULL;

/* ── Lightweight Zero-Dependency JSON Parser Helpers ─────────────────────── */

/**
 * @brief Find and extract a string value for a given key in a JSON object.
 * Format expected: "key" \s* : \s* "value"
 */
static bool json_extract_string(const char *json, const char *key, char *out_val, size_t max_len)
{
    if (!json || !key || !out_val || max_len == 0) return false;
    out_val[0] = '\0';

    char search_key[64];
    snprintf(search_key, sizeof(search_key), "\"%s\"", key);

    const char *p = strstr(json, search_key);
    if (!p) return false;

    p += strlen(search_key);
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != ':') return false;
    p++;
    while (*p && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) p++;
    if (*p != '\"') return false;
    p++; /* skip opening quote */

    size_t idx = 0;
    while (*p && *p != '\"' && idx < max_len - 1) {
        if (*p == '\\' && *(p + 1)) {
            p++; /* skip backslash escape */
        }
        out_val[idx++] = *p++;
    }
    out_val[idx] = '\0';
    return (*p == '\"');
}

/**
 * @brief Validate application ID syntax: alphanumeric, dot, dash, underscore only.
 */
static bool is_valid_app_id(const char *id)
{
    if (!id || strlen(id) == 0 || strlen(id) >= ATLAS_APP_ID_MAX_LEN) return false;
    for (size_t i = 0; id[i] != '\0'; i++) {
        char c = id[i];
        if (!isalnum((unsigned char)c) && c != '.' && c != '-' && c != '_') {
            return false;
        }
    }
    return true;
}

/* ── Public API Implementation ───────────────────────────────────────────── */

esp_err_t app_manager_init(void)
{
    if (!s_app_mutex) {
        s_app_mutex = xSemaphoreCreateMutex();
        if (!s_app_mutex) return ESP_ERR_NO_MEM;
    }

    if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    memset(s_apps, 0, sizeof(s_apps));
    s_app_count = 0;

    /* ── Register Genuine Built-in Applications ──────────────────────────── */
    /*
     * ONLY genuinely implemented applications are registered.
     * Hardcoded un-implemented placeholder cards (APKG Store, NovaSlate, Music)
     * are completely removed.
     */
    atlas_app_t *settings = &s_apps[s_app_count++];
    snprintf(settings->app_id, sizeof(settings->app_id), "settings");
    snprintf(settings->name, sizeof(settings->name), "Settings");
    snprintf(settings->version, sizeof(settings->version), "1.0.0");
    snprintf(settings->tagline, sizeof(settings->tagline), "System & Telemetry");
    settings->type          = ATLAS_APP_TYPE_BUILTIN;
    settings->runtime       = ATLAS_RUNTIME_BUILTIN;
    snprintf(settings->runtime_str, sizeof(settings->runtime_str), "builtin");
    settings->is_installed  = true;
    settings->is_launchable = true;
    settings->launch_fn     = screen_settings_run;

    xSemaphoreGive(s_app_mutex);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Application Manager initialized with %u built-in app(s).",
                   (unsigned int)s_app_count);
    return ESP_OK;
}

esp_err_t app_manager_scan_installed(void)
{
    if (!atlas_sdcard_is_mounted()) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "SD card not mounted — installed app scan skipped.");
        return ESP_OK;
    }

    if (!s_app_mutex) {
        esp_err_t err = app_manager_init();
        if (err != ESP_OK) return err;
    }

    /* Ensure target directory exists */
    struct stat st_dir;
    if (stat(APPS_INSTALLED_DIR, &st_dir) != 0) {
        mkdir(APPS_INSTALLED_DIR, 0777);
        return ESP_OK;
    }

    DIR *dir = opendir(APPS_INSTALLED_DIR);
    if (!dir) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Failed to open directory %s", APPS_INSTALLED_DIR);
        return ESP_OK;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Scanning %s for installed packages...", APPS_INSTALLED_DIR);

    char *json_buf = (char *)malloc(MANIFEST_MAX_FILE_SIZE + 1);
    if (!json_buf) {
        closedir(dir);
        return ESP_ERR_NO_MEM;
    }

    struct dirent *de;
    size_t discovered = 0;

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;

        char app_dir[ATLAS_APP_PATH_MAX_LEN];
        snprintf(app_dir, sizeof(app_dir), "%s/%s", APPS_INSTALLED_DIR, de->d_name);

        struct stat st_entry;
        if (stat(app_dir, &st_entry) != 0 || !S_ISDIR(st_entry.st_mode)) {
            continue; /* Not a directory */
        }

        /* 1. Locate manifest.json */
        char manifest_path[ATLAS_APP_PATH_MAX_LEN + 16];
        snprintf(manifest_path, sizeof(manifest_path), "%s/manifest.json", app_dir);

        struct stat st_manifest;
        if (stat(manifest_path, &st_manifest) != 0) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' missing manifest.json — skipping.", de->d_name);
            continue;
        }

        /* 2. Validate Manifest Size Limit */
        if (st_manifest.st_size <= 0 || st_manifest.st_size > MANIFEST_MAX_FILE_SIZE) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' invalid manifest size (%ld bytes) — skipping.",
                           de->d_name, (long)st_manifest.st_size);
            continue;
        }

        /* 3. Read Manifest Content */
        FILE *f = fopen(manifest_path, "r");
        if (!f) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Cannot open %s — skipping.", manifest_path);
            continue;
        }

        size_t bytes_read = fread(json_buf, 1, st_manifest.st_size, f);
        fclose(f);
        json_buf[bytes_read] = '\0';

        /* 4. Validate JSON boundaries */
        const char *first_brace = strchr(json_buf, '{');
        const char *last_brace  = strrchr(json_buf, '}');
        if (!first_brace || !last_brace || last_brace <= first_brace) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' malformed JSON syntax — skipping.", de->d_name);
            continue;
        }

        /* 5. Extract Required Fields */
        char app_id[ATLAS_APP_ID_MAX_LEN];
        char name[ATLAS_APP_NAME_MAX_LEN];
        char version[ATLAS_APP_VERSION_MAX_LEN];
        char runtime[16];
        char entrypoint[ATLAS_APP_ENTRYPOINT_MAX_LEN];
        char tagline[ATLAS_APP_TAGLINE_MAX_LEN];

        if (!json_extract_string(json_buf, "app_id", app_id, sizeof(app_id)) ||
            !json_extract_string(json_buf, "name", name, sizeof(name)) ||
            !json_extract_string(json_buf, "version", version, sizeof(version)) ||
            !json_extract_string(json_buf, "runtime", runtime, sizeof(runtime)) ||
            !json_extract_string(json_buf, "entrypoint", entrypoint, sizeof(entrypoint))) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' missing required manifest fields — skipping.", de->d_name);
            continue;
        }

        /* Optional tagline */
        if (!json_extract_string(json_buf, "tagline", tagline, sizeof(tagline))) {
            snprintf(tagline, sizeof(tagline), "v%s (%s)", version, runtime);
        }

        /* 6. Validate App ID */
        if (!is_valid_app_id(app_id)) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' invalid app_id '%s' — skipping.", de->d_name, app_id);
            continue;
        }

        /* 7. Security: Check Path Traversal in entrypoint */
        if (strstr(entrypoint, "..") != NULL ||
            entrypoint[0] == '/' || entrypoint[0] == '\\' ||
            strchr(entrypoint, ':') != NULL) {
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "Security: Path traversal attempt in '%s' (%s) — REJECTED.",
                            app_id, entrypoint);
            continue;
        }

        /* 8. Verify Entrypoint File Existence */
        char full_entrypoint[ATLAS_APP_PATH_MAX_LEN + 64];
        snprintf(full_entrypoint, sizeof(full_entrypoint), "%s/%s", app_dir, entrypoint);
        struct stat st_ep;
        if (stat(full_entrypoint, &st_ep) != 0 || S_ISDIR(st_ep.st_mode)) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' entrypoint '%s' not found on disk — skipping.",
                           app_id, full_entrypoint);
            continue;
        }

        /* 9. Disallow SD card packages from claiming 'builtin' runtime */
        if (strcasecmp(runtime, "builtin") == 0) {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' invalid runtime 'builtin' for SD package — skipping.", app_id);
            continue;
        }

        /* 10. Check Duplicate IDs / Collision with Built-ins */
        if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) continue;

        bool duplicate = false;
        for (size_t i = 0; i < s_app_count; i++) {
            if (strncmp(s_apps[i].app_id, app_id, sizeof(s_apps[i].app_id)) == 0 ||
                (strcmp(s_apps[i].app_id, "settings") == 0 && strcmp(app_id, "org.atlas.settings") == 0)) {
                duplicate = true;
                break;
            }
        }
        if (duplicate) {
            xSemaphoreGive(s_app_mutex);
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Duplicate app_id '%s' detected — skipping.", app_id);
            continue;
        }

        if (s_app_count >= ATLAS_APP_MAX_COUNT) {
            xSemaphoreGive(s_app_mutex);
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "App catalogue full (%d max) — cannot register '%s'.",
                           ATLAS_APP_MAX_COUNT, app_id);
            break;
        }

        /* 11. Register Validated Package */
        atlas_app_t *app = &s_apps[s_app_count++];
        snprintf(app->app_id, sizeof(app->app_id), "%s", app_id);
        snprintf(app->name, sizeof(app->name), "%s", name);
        snprintf(app->version, sizeof(app->version), "%s", version);
        snprintf(app->tagline, sizeof(app->tagline), "%s", tagline);
        snprintf(app->entrypoint, sizeof(app->entrypoint), "%s", entrypoint);
        snprintf(app->app_dir, sizeof(app->app_dir), "%s", app_dir);
        app->type         = ATLAS_APP_TYPE_INSTALLED;
        app->is_installed = true;
        app->launch_fn    = NULL;

        /* Check runtime compatibility:
         * In Milestone 2, native dynamic loading and WASM VMs are not implemented.
         * Thus, third-party runtimes are marked is_launchable = false. */
        if (strcasecmp(runtime, "wasm") == 0) {
            app->runtime = ATLAS_RUNTIME_WASM;
            snprintf(app->runtime_str, sizeof(app->runtime_str), "wasm");
            app->is_launchable = true;
        } else if (strcasecmp(runtime, "native") == 0) {
            app->runtime = ATLAS_RUNTIME_NATIVE;
            snprintf(app->runtime_str, sizeof(app->runtime_str), "native");
            app->is_launchable = true;
        } else {
            app->runtime = ATLAS_RUNTIME_UNKNOWN;
            snprintf(app->runtime_str, sizeof(app->runtime_str), "%s", runtime);
            app->is_launchable = false;
        }

        /* 12. Check optional icon file */
        char icon_check[ATLAS_APP_PATH_MAX_LEN + 16];
        snprintf(icon_check, sizeof(icon_check), "%s/icon.raw", app_dir);
        if (access(icon_check, F_OK) == 0) {
            snprintf(app->icon_path, sizeof(app->icon_path), "%s", icon_check);
        } else {
            snprintf(icon_check, sizeof(icon_check), "%s/icon.png", app_dir);
            if (access(icon_check, F_OK) == 0) {
                snprintf(app->icon_path, sizeof(app->icon_path), "%s", icon_check);
            }
        }

        xSemaphoreGive(s_app_mutex);
        discovered++;
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Discovered & Registered package: [%s] '%s' (%s, launchable:%d)",
                       app_id, name, runtime, app->is_launchable ? 1 : 0);
    }

    closedir(dir);
    free(json_buf);
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Scan completed: %u package(s) installed. Total catalogue: %u.",
                   (unsigned int)discovered, (unsigned int)s_app_count);

    /* Notify UI and services of catalogue change */
    atlas_event_post(ATLAS_EVENT_APP_CATALOGUE_CHANGED, NULL, 0, 0);

    return ESP_OK;
}

size_t app_manager_get_count(void)
{
    if (!s_app_mutex) app_manager_init();
    if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) return 0;
    size_t count = s_app_count;
    xSemaphoreGive(s_app_mutex);
    return count;
}

esp_err_t app_manager_get_at(size_t index, atlas_app_t *out_app)
{
    if (!out_app) return ESP_ERR_INVALID_ARG;
    if (!s_app_mutex) app_manager_init();
    if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    if (index >= s_app_count) {
        xSemaphoreGive(s_app_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    *out_app = s_apps[index];
    xSemaphoreGive(s_app_mutex);
    return ESP_OK;
}

esp_err_t app_manager_get_by_id(const char *app_id, atlas_app_t *out_app)
{
    if (!app_id || !out_app) return ESP_ERR_INVALID_ARG;
    if (!s_app_mutex) app_manager_init();
    if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    for (size_t i = 0; i < s_app_count; i++) {
        if (strncmp(s_apps[i].app_id, app_id, sizeof(s_apps[i].app_id)) == 0) {
            *out_app = s_apps[i];
            xSemaphoreGive(s_app_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_app_mutex);
    return ESP_ERR_NOT_FOUND;
}

/* ── Dynamic WebAssembly Host Syscalls (Exported to "env") ───────────────── */

m3ApiRawFunction(AtlasSyscall_DrawPixel)
{
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(uint32_t, color)

    uint16_t *fb = atlas_display_get_framebuffer();
    atlas_gfx_draw_pixel(fb, x, y, (uint16_t)color);
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_FillRect)
{
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArg(int32_t, w)
    m3ApiGetArg(int32_t, h)
    m3ApiGetArg(uint32_t, color)

    uint16_t *fb = atlas_display_get_framebuffer();
    atlas_gfx_fill_rect(fb, x, y, w, h, (uint16_t)color);
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_DrawString)
{
    m3ApiGetArg(int32_t, x)
    m3ApiGetArg(int32_t, y)
    m3ApiGetArgMem(const char *, str)
    m3ApiGetArg(uint32_t, fg)
    m3ApiGetArg(uint32_t, bg)
    m3ApiGetArg(int32_t, scale)

    uint16_t *fb = atlas_display_get_framebuffer();
    atlas_gfx_draw_string(fb, x, y, str, (uint16_t)fg, (uint16_t)bg, scale);
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_DisplayFlush)
{
    const uint16_t *fb = atlas_display_get_framebuffer();
    atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
    atlas_display_wait_flush();
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_GetInput)
{
    m3ApiGetArgMem(int32_t *, out_joy_x)
    m3ApiGetArgMem(int32_t *, out_joy_y)
    m3ApiGetArgMem(uint32_t *, out_btn_mask)

    atlas_input_state_t in = atlas_input_get_state();
    *out_joy_x = in.joy_x;
    *out_joy_y = in.joy_y;
    uint32_t mask = 0;
    if (in.btn_a) mask |= 0x01;
    if (in.btn_b) mask |= 0x02;
    if (in.btn_c) mask |= 0x04;
    if (in.btn_d) mask |= 0x08;
    *out_btn_mask = mask;
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_PlayBeep)
{
    m3ApiGetArg(uint32_t, raw_freq)
    m3ApiGetArg(int32_t, duration_ms)

    float freq = 0.0f;
    memcpy(&freq, &raw_freq, sizeof(freq));
    atlas_audio_play_beep(freq, duration_ms);
    m3ApiSuccess();
}

m3ApiRawFunction(AtlasSyscall_DelayMs)
{
    m3ApiGetArg(uint32_t, ms)

    vTaskDelay(pdMS_TO_TICKS(ms));
    m3ApiSuccess();
}

/* ── Dynamic SD Card Application Loader ──────────────────────────────────── */

static esp_err_t launch_dynamic_app(const atlas_app_t *app)
{
    char full_path[ATLAS_APP_PATH_MAX_LEN + 64];
    snprintf(full_path, sizeof(full_path), "%s/%s", app->app_dir, app->entrypoint);

    struct stat st;
    if (stat(full_path, &st) != 0 || st.st_size <= 0) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "App entrypoint file not found: %s", full_path);
        return ESP_ERR_NOT_FOUND;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Loading dynamic app '%s' (%lu bytes) from SD card into PSRAM...",
                   app->name, (unsigned long)st.st_size);

    FILE *f = fopen(full_path, "rb");
    if (!f) {
        return ESP_FAIL;
    }

    uint8_t *wasm_buf = (uint8_t *)heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM);
    if (!wasm_buf) {
        wasm_buf = (uint8_t *)malloc(st.st_size);
    }
    if (!wasm_buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }

    size_t read_bytes = fread(wasm_buf, 1, st.st_size, f);
    fclose(f);

    if (read_bytes != (size_t)st.st_size) {
        free(wasm_buf);
        return ESP_FAIL;
    }

    /* Check for WebAssembly Magic Number: \0asm */
    if (st.st_size >= 4 && wasm_buf[0] == 0x00 && wasm_buf[1] == 0x61 && wasm_buf[2] == 0x73 && wasm_buf[3] == 0x6D) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Initializing Wasm3 virtual machine...");
        IM3Environment env = m3_NewEnvironment();
        if (!env) {
            free(wasm_buf);
            return ESP_ERR_NO_MEM;
        }

        IM3Runtime runtime = m3_NewRuntime(env, 64 * 1024, NULL);
        if (!runtime) {
            m3_FreeEnvironment(env);
            free(wasm_buf);
            return ESP_ERR_NO_MEM;
        }

        IM3Module module = NULL;
        M3Result res = m3_ParseModule(env, &module, wasm_buf, st.st_size);
        if (res) {
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "WASM parse failed: %s", res);
            m3_FreeRuntime(runtime);
            m3_FreeEnvironment(env);
            free(wasm_buf);
            return ESP_FAIL;
        }

        res = m3_LoadModule(runtime, module);
        if (res) {
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "WASM load failed: %s", res);
            m3_FreeRuntime(runtime);
            m3_FreeEnvironment(env);
            free(wasm_buf);
            return ESP_FAIL;
        }

        /* Link host syscalls */
        m3_LinkRawFunction(module, "env", "atlas_api_draw_pixel", "v(iii)", &AtlasSyscall_DrawPixel);
        m3_LinkRawFunction(module, "env", "atlas_api_fill_rect", "v(iiiii)", &AtlasSyscall_FillRect);
        m3_LinkRawFunction(module, "env", "atlas_api_draw_string", "v(ii*iii)", &AtlasSyscall_DrawString);
        m3_LinkRawFunction(module, "env", "atlas_api_display_flush", "v()", &AtlasSyscall_DisplayFlush);
        m3_LinkRawFunction(module, "env", "atlas_api_get_input", "v(***)", &AtlasSyscall_GetInput);
        m3_LinkRawFunction(module, "env", "atlas_api_play_beep", "v(fi)", &AtlasSyscall_PlayBeep);
        m3_LinkRawFunction(module, "env", "atlas_api_delay_ms", "v(i)", &AtlasSyscall_DelayMs);

        IM3Function func = NULL;
        m3_FindFunction(&func, runtime, "_start");
        if (!func) m3_FindFunction(&func, runtime, "main");
        if (!func) m3_FindFunction(&func, runtime, "atlas_main");

        if (func) {
            ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Executing WASM application entrypoint...");
            res = m3_CallV(func);
            if (res) {
                ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "WASM application trapped: %s", res);
            }
        } else {
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "No valid entrypoint (_start / main / atlas_main) in module");
        }

        m3_FreeRuntime(runtime);
        m3_FreeEnvironment(env);
        free(wasm_buf);
        return ESP_OK;
    }

    /* Fallback: Dynamic standalone application viewer */
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Dynamic application payload loaded: %s (%lu bytes)", app->name, (unsigned long)st.st_size);
    uint16_t *fb = atlas_display_get_framebuffer();
    if (fb) {
        bool running = true;
        bool last_b = true;
        bool last_c = true;
        while (running) {
            atlas_input_state_t in = atlas_input_get_state();
            if ((in.btn_b && !last_b) || (in.btn_c && !last_c)) {
                break;
            }
            last_b = in.btn_b;
            last_c = in.btn_c;

            atlas_gfx_clear(fb, COLOR_BG_DARK);
            atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 24, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 23, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
            atlas_gfx_draw_string(fb, 12, 7, app->name, COLOR_CYAN, 0x0001, 1);

            atlas_gfx_fill_rect(fb, 20, 50, 280, 110, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 20, 50, 280, 110, COLOR_BORDER_NORM);

            char line[64];
            snprintf(line, sizeof(line), "App ID     : %s", app->app_id);
            atlas_gfx_draw_string(fb, 30, 65, line, COLOR_WHITE, 0x0001, 1);

            snprintf(line, sizeof(line), "Entrypoint : %s (%lu B)", app->entrypoint, (unsigned long)st.st_size);
            atlas_gfx_draw_string(fb, 30, 85, line, COLOR_GREEN, 0x0001, 1);

            atlas_gfx_draw_string(fb, 30, 105, "Status     : Loaded dynamically from SD", COLOR_TEXT_MAIN, 0x0001, 1);
            atlas_gfx_draw_string(fb, 30, 125, "Source     : /sdcard/apps/installed/", COLOR_YELLOW, 0x0001, 1);

            atlas_gfx_fill_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
            atlas_gfx_draw_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
            atlas_gfx_draw_string(fb, 12, 222, "[B/Menu] Return to Launcher", COLOR_YELLOW, 0x0001, 1);

            atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
            atlas_display_wait_flush();
            vTaskDelay(pdMS_TO_TICKS(40));
        }
    }

    free(wasm_buf);
    return ESP_OK;
}

#define APPS_AVAILABLE_DIR "/sdcard/packages/available"

bool app_manager_is_protected(const char *app_id)
{
    if (!app_id) return false;
    return (strcmp(app_id, "org.atlas.store") == 0 ||
            strcmp(app_id, "store") == 0 ||
            strcmp(app_id, "org.atlas.settings") == 0 ||
            strcmp(app_id, "settings") == 0);
}

static esp_err_t copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return ESP_FAIL;
    FILE *out = fopen(dst, "wb");
    if (!out) {
        fclose(in);
        return ESP_FAIL;
    }
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
        fwrite(buf, 1, n, out);
    }
    fclose(in);
    fclose(out);
    return ESP_OK;
}

esp_err_t app_manager_install(const char *app_id)
{
    if (!app_id || !is_valid_app_id(app_id)) return ESP_ERR_INVALID_ARG;

    char avail_dir[ATLAS_APP_PATH_MAX_LEN];
    snprintf(avail_dir, sizeof(avail_dir), "%s/%s", APPS_AVAILABLE_DIR, app_id);

    char install_dir[ATLAS_APP_PATH_MAX_LEN];
    snprintf(install_dir, sizeof(install_dir), "%s/%s", APPS_INSTALLED_DIR, app_id);

    struct stat st;
    if (stat(avail_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Package '%s' not found in %s", app_id, APPS_AVAILABLE_DIR);
        return ESP_ERR_NOT_FOUND;
    }

    mkdir(install_dir, 0777);

    char src_manifest[ATLAS_APP_PATH_MAX_LEN + 16];
    char dst_manifest[ATLAS_APP_PATH_MAX_LEN + 16];
    snprintf(src_manifest, sizeof(src_manifest), "%s/manifest.json", avail_dir);
    snprintf(dst_manifest, sizeof(dst_manifest), "%s/manifest.json", install_dir);
    copy_file(src_manifest, dst_manifest);

    char src_bin[ATLAS_APP_PATH_MAX_LEN + 16];
    char dst_bin[ATLAS_APP_PATH_MAX_LEN + 16];
    snprintf(src_bin, sizeof(src_bin), "%s/app.bin", avail_dir);
    snprintf(dst_bin, sizeof(dst_bin), "%s/app.bin", install_dir);
    copy_file(src_bin, dst_bin);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Package '%s' installed successfully to %s", app_id, install_dir);
    app_manager_scan_installed();
    return ESP_OK;
}

typedef struct {
    char app_id[ATLAS_APP_ID_MAX_LEN];
    char name[ATLAS_APP_NAME_MAX_LEN];
    char version[ATLAS_APP_VERSION_MAX_LEN];
    char tagline[ATLAS_APP_TAGLINE_MAX_LEN];
    bool is_installed;
    bool is_protected;
} apkg_store_entry_t;

static esp_err_t launch_apkg_store(void)
{
    uint16_t *fb = atlas_display_get_framebuffer();
    if (!fb) return ESP_FAIL;

    apkg_store_entry_t *entries = (apkg_store_entry_t *)malloc(sizeof(apkg_store_entry_t) * 16);
    if (!entries) return ESP_ERR_NO_MEM;
    memset(entries, 0, sizeof(apkg_store_entry_t) * 16);

    size_t count = 0;

    /* 1. Add protected Factory Apps */
    snprintf(entries[count].app_id, sizeof(entries[count].app_id), "org.atlas.store");
    snprintf(entries[count].name, sizeof(entries[count].name), "APKG Store");
    snprintf(entries[count].version, sizeof(entries[count].version), "1.0.0");
    snprintf(entries[count].tagline, sizeof(entries[count].tagline), "Install & Manage Packages");
    entries[count].is_installed = true;
    entries[count].is_protected = true;
    count++;

    snprintf(entries[count].app_id, sizeof(entries[count].app_id), "org.atlas.settings");
    snprintf(entries[count].name, sizeof(entries[count].name), "Settings");
    snprintf(entries[count].version, sizeof(entries[count].version), "1.0.0");
    snprintf(entries[count].tagline, sizeof(entries[count].tagline), "System & Telemetry");
    entries[count].is_installed = true;
    entries[count].is_protected = true;
    count++;

    /* 2. Scan /sdcard/packages/available/ */
    DIR *dir = opendir(APPS_AVAILABLE_DIR);
    if (dir) {
        struct dirent *de;
        char *json_buf = (char *)malloc(MANIFEST_MAX_FILE_SIZE + 1);
        while ((de = readdir(dir)) != NULL && count < 16) {
            if (de->d_name[0] == '.') continue;
            if (strcmp(de->d_name, "org.atlas.store") == 0 ||
                strcmp(de->d_name, "org.atlas.settings") == 0) continue;

            char manifest_path[ATLAS_APP_PATH_MAX_LEN + 16];
            snprintf(manifest_path, sizeof(manifest_path), "%s/%s/manifest.json", APPS_AVAILABLE_DIR, de->d_name);

            FILE *f = fopen(manifest_path, "r");
            if (!f) continue;
            size_t n = fread(json_buf, 1, MANIFEST_MAX_FILE_SIZE, f);
            fclose(f);
            json_buf[n] = '\0';

            char app_id[ATLAS_APP_ID_MAX_LEN] = {0};
            char name[ATLAS_APP_NAME_MAX_LEN] = {0};
            char version[ATLAS_APP_VERSION_MAX_LEN] = {0};

            if (json_extract_string(json_buf, "app_id", app_id, sizeof(app_id)) &&
                json_extract_string(json_buf, "name", name, sizeof(name)) &&
                json_extract_string(json_buf, "version", version, sizeof(version))) {

                char tagline[ATLAS_APP_TAGLINE_MAX_LEN] = {0};
                if (!json_extract_string(json_buf, "tagline", tagline, sizeof(tagline))) {
                    snprintf(tagline, sizeof(tagline), "v%s", version);
                }

                snprintf(entries[count].app_id, sizeof(entries[count].app_id), "%s", app_id);
                snprintf(entries[count].name, sizeof(entries[count].name), "%s", name);
                snprintf(entries[count].version, sizeof(entries[count].version), "%s", version);
                snprintf(entries[count].tagline, sizeof(entries[count].tagline), "%s", tagline);

                /* Check if installed */
                char chk_path[ATLAS_APP_PATH_MAX_LEN + 16];
                snprintf(chk_path, sizeof(chk_path), "%s/%s/manifest.json", APPS_INSTALLED_DIR, app_id);
                struct stat st;
                entries[count].is_installed = (stat(chk_path, &st) == 0 && st.st_size > 0);
                entries[count].is_protected = false;
                count++;
            }
        }
        if (json_buf) free(json_buf);
        closedir(dir);
    }

    /* 3. Interactive Store Loop */
    int selected_idx = 0;
    atlas_input_state_t last_in = {0};
    bool running = true;
    char notice_msg[48] = {0};
    int notice_timer = 0;

    while (running) {
        atlas_input_state_t in = atlas_input_get_state();
        bool pressed_up = (in.joy_y < -50 && last_in.joy_y >= -50);
        bool pressed_down = (in.joy_y > 50 && last_in.joy_y <= 50);
        bool pressed_a = (in.btn_a && !last_in.btn_a);
        bool pressed_d = (in.btn_d && !last_in.btn_d);
        bool pressed_b = (in.btn_b && !last_in.btn_b);
        bool pressed_c = (in.btn_c && !last_in.btn_c);
        last_in = in;

        if (pressed_up && selected_idx > 0) {
            selected_idx--;
            atlas_audio_play_beep(1000.0f, 15);
        } else if (pressed_down && selected_idx + 1 < (int)count) {
            selected_idx++;
            atlas_audio_play_beep(1000.0f, 15);
        }

        if (pressed_b || pressed_c) {
            atlas_audio_play_beep(700.0f, 30);
            break;
        }

        /* [A]: Install Package */
        if (pressed_a && count > 0) {
            apkg_store_entry_t *cur = &entries[selected_idx];
            if (!cur->is_installed) {
                esp_err_t ins_err = app_manager_install(cur->app_id);
                if (ins_err == ESP_OK) {
                    cur->is_installed = true;
                    atlas_audio_play_beep(1400.0f, 60);
                    snprintf(notice_msg, sizeof(notice_msg), "INSTALLED: %s", cur->name);
                    notice_timer = 40;
                }
            } else {
                atlas_audio_play_beep(900.0f, 20);
                snprintf(notice_msg, sizeof(notice_msg), "%s ALREADY INSTALLED", cur->name);
                notice_timer = 30;
            }
        }

        /* [D]: Uninstall Package */
        if (pressed_d && count > 0) {
            apkg_store_entry_t *cur = &entries[selected_idx];
            if (cur->is_protected) {
                atlas_audio_play_beep(300.0f, 100);
                snprintf(notice_msg, sizeof(notice_msg), "LOCKED: CANNOT UNINSTALL");
                notice_timer = 40;
            } else if (cur->is_installed) {
                esp_err_t unins_err = app_manager_uninstall(cur->app_id);
                if (unins_err == ESP_OK) {
                    cur->is_installed = false;
                    atlas_audio_play_beep(500.0f, 60);
                    snprintf(notice_msg, sizeof(notice_msg), "UNINSTALLED: %s", cur->name);
                    notice_timer = 40;
                }
            }
        }

        /* ── Render Store UI ─────────────────────────────────────────────── */
        atlas_gfx_clear(fb, COLOR_BG_DARK);

        /* Top Header */
        atlas_gfx_fill_rect(fb, 0, 0, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 0, 25, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 12, 8, "APKG STORE", COLOR_CYAN, COLOR_PANEL_BG, 1);

        uint64_t total_b = 0, free_b = 0;
        atlas_sdcard_get_info(&total_b, &free_b);
        char cap_str[32];
        snprintf(cap_str, sizeof(cap_str), "SD: %llu MB Free", (unsigned long long)(free_b / (1024 * 1024)));
        atlas_gfx_draw_string(fb, 180, 8, cap_str, COLOR_WHITE, COLOR_PANEL_BG, 1);

        /* App Items List */
        int start_y = 32;
        int item_h = 24;
        int max_visible = 5;
        int scroll_offset = 0;
        if (selected_idx >= max_visible) {
            scroll_offset = selected_idx - max_visible + 1;
        }

        for (int i = 0; i < max_visible && (i + scroll_offset) < (int)count; i++) {
            int idx = i + scroll_offset;
            int y = start_y + i * (item_h + 3);
            bool is_sel = (idx == selected_idx);

            uint16_t bg = is_sel ? COLOR_CYAN : COLOR_PANEL_BG;
            uint16_t fg = is_sel ? COLOR_BLACK : COLOR_WHITE;

            atlas_gfx_fill_rect(fb, 12, y, 296, item_h, bg);
            atlas_gfx_draw_rect(fb, 12, y, 296, item_h, is_sel ? COLOR_WHITE : COLOR_BORDER_NORM);

            atlas_gfx_draw_string(fb, 20, y + 6, entries[idx].name, fg, bg, 1);

            /* Badges */
            if (entries[idx].is_protected) {
                atlas_gfx_draw_string(fb, 180, y + 6, "[FACTORY]", is_sel ? COLOR_BLACK : COLOR_CYAN, bg, 1);
            } else if (entries[idx].is_installed) {
                atlas_gfx_draw_string(fb, 180, y + 6, "[INSTALLED]", is_sel ? COLOR_BLACK : COLOR_GREEN, bg, 1);
            } else {
                atlas_gfx_draw_string(fb, 180, y + 6, "[AVAILABLE]", is_sel ? COLOR_BLACK : COLOR_YELLOW, bg, 1);
            }
        }

        /* Detail Card */
        atlas_gfx_fill_rect(fb, 12, 168, 296, 42, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 12, 168, 296, 42, COLOR_BORDER_NORM);

        if (notice_timer > 0) {
            notice_timer--;
            atlas_gfx_draw_string_centered(fb, 182, notice_msg, COLOR_YELLOW, COLOR_PANEL_BG, 1);
        } else if (count > 0) {
            const apkg_store_entry_t *cur = &entries[selected_idx];
            atlas_gfx_draw_string(fb, 20, 172, cur->tagline, COLOR_WHITE, COLOR_PANEL_BG, 1);

            if (cur->is_protected) {
                atlas_gfx_draw_string(fb, 20, 190, "Protected Factory App (Cannot uninstall)", COLOR_TEXT_MUTED, COLOR_PANEL_BG, 1);
            } else if (cur->is_installed) {
                atlas_gfx_draw_string(fb, 20, 190, "Installed! Press [D] to Uninstall", COLOR_RED, COLOR_PANEL_BG, 1);
            } else {
                atlas_gfx_draw_string(fb, 20, 190, "Available! Press [A] to Install Package", COLOR_GREEN, COLOR_PANEL_BG, 1);
            }
        }

        /* Bottom Footer */
        atlas_gfx_fill_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 26, COLOR_PANEL_BG);
        atlas_gfx_draw_rect(fb, 0, 214, ATLAS_LCD_WIDTH, 1, COLOR_BORDER_NORM);
        atlas_gfx_draw_string(fb, 12, 222, "[A] Install  [D] Uninstall  [B/C] Return Home", COLOR_YELLOW, COLOR_PANEL_BG, 1);

        atlas_display_flush(0, 0, ATLAS_LCD_WIDTH - 1, ATLAS_LCD_HEIGHT - 1, fb);
        atlas_display_wait_flush();
        vTaskDelay(pdMS_TO_TICKS(40));
    }

    free(entries);
    /* Rescan installed apps so Home Launcher picks up any additions/removals */
    app_manager_scan_installed();
    return ESP_OK;
}

esp_err_t app_manager_launch(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;

    atlas_app_t app;
    esp_err_t err = app_manager_get_by_id(app_id, &app);
    if (err != ESP_OK) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Launch requested for unknown app '%s'", app_id);
        return err;
    }

    if (strcmp(app.app_id, "org.atlas.store") == 0 || strcmp(app.app_id, "store") == 0) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Launching dynamic APKG Store browser...");
        return launch_apkg_store();
    }

    if (strcmp(app.app_id, "org.atlas.settings") == 0 || strcmp(app.app_id, "settings") == 0) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Launching System Settings...");
        screen_settings_run();
        return ESP_OK;
    }

    if (!app.is_launchable) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "App '%s' is not launchable (runtime '%s' not supported).",
                       app_id, app.runtime_str);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (app.type == ATLAS_APP_TYPE_BUILTIN && app.launch_fn != NULL) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Executing built-in app handler for '%s'...", app_id);
        app.launch_fn();
        return ESP_OK;
    }

    /* All installed applications loaded dynamically from SD card! */
    return launch_dynamic_app(&app);
}

/* ── Directory Cleanup Helper ────────────────────────────────────────────── */

static esp_err_t rm_dir_recursive(const char *path)
{
    DIR *dir = opendir(path);
    if (!dir) {
        return (unlink(path) == 0) ? ESP_OK : ESP_FAIL;
    }

    struct dirent *de;
    while ((de = readdir(dir)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) {
            continue;
        }

        char child[ATLAS_APP_PATH_MAX_LEN + 64];
        snprintf(child, sizeof(child), "%s/%s", path, de->d_name);

        struct stat st;
        if (stat(child, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                rm_dir_recursive(child);
            } else {
                unlink(child);
            }
        }
    }
    closedir(dir);
    return (rmdir(path) == 0) ? ESP_OK : ESP_FAIL;
}

esp_err_t app_manager_uninstall(const char *app_id)
{
    if (!app_id || !is_valid_app_id(app_id)) return ESP_ERR_INVALID_ARG;
    if (!s_app_mutex) app_manager_init();
    if (xSemaphoreTake(s_app_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    int found_idx = -1;
    for (size_t i = 0; i < s_app_count; i++) {
        if (strncmp(s_apps[i].app_id, app_id, sizeof(s_apps[i].app_id)) == 0) {
            found_idx = (int)i;
            break;
        }
    }

    if (found_idx < 0) {
        xSemaphoreGive(s_app_mutex);
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Uninstall failed: app '%s' not registered.", app_id);
        return ESP_ERR_NOT_FOUND;
    }

    /* Protected factory applications & built-ins CANNOT be uninstalled */
    if (app_manager_is_protected(app_id) || s_apps[found_idx].type == ATLAS_APP_TYPE_BUILTIN) {
        xSemaphoreGive(s_app_mutex);
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_APP, "Cannot uninstall protected factory application '%s'.", app_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    /* Delete package directory from SD card */
    char app_dir[ATLAS_APP_PATH_MAX_LEN];
    snprintf(app_dir, sizeof(app_dir), "%s", s_apps[found_idx].app_dir);

    /* Double check path is inside /sdcard/apps/installed/ */
    if (strncmp(app_dir, APPS_INSTALLED_DIR, strlen(APPS_INSTALLED_DIR)) == 0 &&
        strstr(app_dir, "..") == NULL) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Removing package files at %s...", app_dir);
        rm_dir_recursive(app_dir);
    }

    /* Shift remaining apps down */
    for (size_t i = (size_t)found_idx; i + 1 < s_app_count; i++) {
        s_apps[i] = s_apps[i + 1];
    }
    s_app_count--;
    memset(&s_apps[s_app_count], 0, sizeof(atlas_app_t));

    xSemaphoreGive(s_app_mutex);
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Package '%s' uninstalled successfully.", app_id);

    atlas_event_post(ATLAS_EVENT_APP_CATALOGUE_CHANGED, NULL, 0, 0);
    return ESP_OK;
}

esp_err_t app_manager_ensure_boot_os(void)
{
    return atlas_ota_validate_boot();
}

