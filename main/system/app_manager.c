/**
 * @file app_manager.c
 * @brief Dynamic Application Manager Implementation for ATLAS OS
 */
#include "app_manager.h"
#include "atlas_log.h"
#include "atlas_event.h"
#include "atlas_sdcard.h"
#include "screen_settings.h"

#include <stdio.h>
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
            if (strncmp(s_apps[i].app_id, app_id, sizeof(s_apps[i].app_id)) == 0) {
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
            app->is_launchable = false;
        } else if (strcasecmp(runtime, "native") == 0) {
            app->runtime = ATLAS_RUNTIME_NATIVE;
            snprintf(app->runtime_str, sizeof(app->runtime_str), "native");
            app->is_launchable = false;
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

esp_err_t app_manager_launch(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;

    atlas_app_t app;
    esp_err_t err = app_manager_get_by_id(app_id, &app);
    if (err != ESP_OK) {
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_APP, "Launch requested for unknown app '%s'", app_id);
        return err;
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

    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t app_manager_ensure_boot_os(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_APP, "Running Atlas OS on partition '%s' (0x%lx)",
                       running->label, (unsigned long)running->address);
    }
    return ESP_OK;
}

