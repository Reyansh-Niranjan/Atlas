/**
 * @file app_registry.c
 * @brief NVS Application Registry, In-Firmware Dispatch, and Firmware Update Engine
 *
 * See app_registry.h for the architecture rationale.
 */
#include "app_registry.h"
#include "screen_settings.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_registry";
#define NVS_NAMESPACE       "atlas_apps"
#define NVS_KEY_APP_COUNT   "app_cnt"

static SemaphoreHandle_t s_reg_mutex = NULL;

/* ── Default System Applications ─────────────────────────────────────────── */
/*
 * flash_offset and size are zeroed for built-in apps; they have no associated
 * SD binary and are dispatched via compiled-in handlers, not OTA flashing.
 */
static const atlas_app_entry_t s_default_apps[] = {
    {
        .app_id       = APP_ID_SETTINGS,
        .app_name     = "Settings",
        .tagline      = "Telemetry & Cal",
        .version      = "1.0.0",
        .bin_path     = "",
        .flash_offset = 0,
        .size         = 0,
        .is_installed = true,
        .is_staged    = false,
    },
};

/* ── In-firmware handler registry ────────────────────────────────────────── */
static const struct { const char *id; atlas_app_handler_t fn; } s_handlers[] = {
    { APP_ID_SETTINGS,   screen_settings_run },
};

/* ── Internal helpers ────────────────────────────────────────────────────── */

/* Open NVS and read the app count; out_count = 0 on any error. */
static esp_err_t nvs_open_read_count(nvs_handle_t *out_h, uint32_t *out_count)
{
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, out_h);
    if (err != ESP_OK) {
        *out_count = 0;
        return err;
    }
    *out_count = 0;
    nvs_get_u32(*out_h, NVS_KEY_APP_COUNT, out_count);
    return ESP_OK;
}

/* ── Public API ──────────────────────────────────────────────────────────── */

esp_err_t app_registry_init(void)
{
    if (!s_reg_mutex) {
        s_reg_mutex = xSemaphoreCreateMutex();
        if (!s_reg_mutex) return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace %s: %s", NVS_NAMESPACE, esp_err_to_name(err));
        return err;
    }

    uint32_t count = 0;
    err = nvs_get_u32(handle, NVS_KEY_APP_COUNT, &count);
    if (err == ESP_ERR_NVS_NOT_FOUND || count == 0) {
        ESP_LOGI(TAG, "Empty NVS app table. Seeding %u default system applications...",
                 (unsigned int)(sizeof(s_default_apps) / sizeof(s_default_apps[0])));
        size_t def_count = sizeof(s_default_apps) / sizeof(s_default_apps[0]);
        for (size_t i = 0; i < def_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
            nvs_set_blob(handle, key, &s_default_apps[i], sizeof(atlas_app_entry_t));
        }
        nvs_set_u32(handle, NVS_KEY_APP_COUNT, (uint32_t)def_count);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Seeded %u default applications to NVS.", (unsigned int)def_count);
    } else {
        ESP_LOGI(TAG, "Loaded app registry from NVS (%lu apps).", (unsigned long)count);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_registry_get_all(atlas_app_entry_t *out_entries,
                               size_t max_entries, size_t *out_count)
{
    if (!out_entries || max_entries == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t handle;
    uint32_t count = 0;
    esp_err_t err = nvs_open_read_count(&handle, &count);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    size_t loaded = 0;
    for (uint32_t i = 0; i < count && loaded < max_entries; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        size_t req = sizeof(atlas_app_entry_t);
        if (nvs_get_blob(handle, key, &out_entries[loaded], &req) == ESP_OK) {
            out_entries[loaded].is_staged = false; /* not used in this release */
            loaded++;
        }
    }

    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);
    if (out_count) *out_count = loaded;
    return ESP_OK;
}

esp_err_t app_registry_get(const char *app_id, atlas_app_entry_t *out_entry)
{
    if (!app_id || !out_entry) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t handle;
    uint32_t count = 0;
    esp_err_t err = nvs_open_read_count(&handle, &count);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        atlas_app_entry_t entry;
        size_t req = sizeof(entry);
        if (nvs_get_blob(handle, key, &entry, &req) == ESP_OK) {
            if (strncmp(entry.app_id, app_id, sizeof(entry.app_id)) == 0) {
                entry.is_staged = false;
                *out_entry = entry;
                found = true;
                break;
            }
        }
    }

    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);
    return found ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_registry_register(const atlas_app_entry_t *entry)
{
    if (!entry || strlen(entry->app_id) == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, NVS_KEY_APP_COUNT, &count);

    int target_slot = -1;
    for (uint32_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        atlas_app_entry_t cur;
        size_t req = sizeof(cur);
        if (nvs_get_blob(handle, key, &cur, &req) == ESP_OK) {
            if (strncmp(cur.app_id, entry->app_id, sizeof(cur.app_id)) == 0) {
                target_slot = (int)i;
                break;
            }
        }
    }

    if (target_slot < 0) {
        if (count >= APP_REGISTRY_MAX_APPS) {
            nvs_close(handle);
            xSemaphoreGive(s_reg_mutex);
            return ESP_ERR_NO_MEM;
        }
        target_slot = (int)count;
        count++;
        nvs_set_u32(handle, NVS_KEY_APP_COUNT, count);
    }

    char key[16];
    snprintf(key, sizeof(key), "app_%d", target_slot);

    /* Force is_staged=false in NVS; the field is unused in this release. */
    atlas_app_entry_t to_store = *entry;
    to_store.is_staged = false;
    nvs_set_blob(handle, key, &to_store, sizeof(atlas_app_entry_t));
    nvs_commit(handle);
    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);

    ESP_LOGI(TAG, "Registered/updated: %s (%s)", entry->app_name, entry->app_id);
    return ESP_OK;
}

esp_err_t app_registry_uninstall(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    /* Remove SD binary if present (non-fatal if absent) */
    char bin_path[128];
    snprintf(bin_path, sizeof(bin_path), "/sdcard/apps/%s.bin", app_id);
    unlink(bin_path);

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, NVS_KEY_APP_COUNT, &count);

    bool updated = false;
    for (uint32_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        atlas_app_entry_t cur;
        size_t req = sizeof(cur);
        if (nvs_get_blob(handle, key, &cur, &req) == ESP_OK) {
            if (strncmp(cur.app_id, app_id, sizeof(cur.app_id)) == 0) {
                cur.is_installed = false;
                nvs_set_blob(handle, key, &cur, sizeof(cur));
                nvs_commit(handle);
                updated = true;
                ESP_LOGI(TAG, "Marked '%s' uninstalled.", app_id);
                break;
            }
        }
    }

    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);
    return updated ? ESP_OK : ESP_ERR_NOT_FOUND;
}

esp_err_t app_registry_scan_sdcard(void)
{
    DIR *dir = opendir("/sdcard/apps");
    if (!dir) {
        /* Create the directory so future drops work; absence is non-fatal. */
        mkdir("/sdcard/apps", 0777);
        dir = opendir("/sdcard/apps");
        if (!dir) return ESP_OK; /* no SD or no apps dir — not an error */
    }

    ESP_LOGI(TAG, "Scanning /sdcard/apps/ for .bin files (metadata only)...");
    struct dirent *de;
    int found = 0;

    while ((de = readdir(dir)) != NULL) {
        if (de->d_name[0] == '.') continue;
        size_t len = strlen(de->d_name);
        if (len <= 4 || strcmp(de->d_name + len - 4, ".bin") != 0) continue;

        char app_id[32] = {0};
        size_t id_len = len - 4;
        if (id_len >= sizeof(app_id)) id_len = sizeof(app_id) - 1;
        memcpy(app_id, de->d_name, id_len);

        char full_path[128];
        snprintf(full_path, sizeof(full_path), "/sdcard/apps/%s", de->d_name);
        struct stat st;
        uint32_t fsize = (stat(full_path, &st) == 0) ? (uint32_t)st.st_size : 0;

        /* Capitalise first letter for display name */
        char app_name[32];
        snprintf(app_name, sizeof(app_name), "%s", app_id);
        if (app_name[0] >= 'a' && app_name[0] <= 'z') app_name[0] -= 0x20;

        atlas_app_entry_t app = {0};
        snprintf(app.app_id,    sizeof(app.app_id),    "%s", app_id);
        snprintf(app.app_name,  sizeof(app.app_name),  "%s", app_name);
        snprintf(app.tagline,   sizeof(app.tagline),   "SD File (%lu KB) — not launchable",
                 (unsigned long)(fsize / 1024));
        snprintf(app.version,   sizeof(app.version),   "unknown");
        snprintf(app.bin_path,  sizeof(app.bin_path),  "%s", full_path);
        app.size         = fsize;
        app.flash_offset = 0; /* unused */
        app.is_installed = true;
        app.is_staged    = false;

        app_registry_register(&app);
        found++;
        ESP_LOGI(TAG, "Indexed SD file: %s (%lu KB)", full_path, (unsigned long)(fsize / 1024));
    }
    closedir(dir);
    ESP_LOGI(TAG, "SD scan complete: %d file(s) indexed.", found);
    return ESP_OK;
}

atlas_app_handler_t app_registry_get_handler(const char *app_id)
{
    if (!app_id) return NULL;
    for (size_t i = 0; i < sizeof(s_handlers) / sizeof(s_handlers[0]); i++) {
        if (strcmp(s_handlers[i].id, app_id) == 0) {
            return s_handlers[i].fn;
        }
    }
    return NULL;
}

esp_err_t app_registry_launch(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;

    atlas_app_entry_t entry;
    esp_err_t err = app_registry_get(app_id, &entry);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "app_registry_launch: '%s' not in registry.", app_id);
        return err;
    }

    atlas_app_handler_t fn = app_registry_get_handler(app_id);
    if (!fn) {
        ESP_LOGW(TAG, "app_registry_launch: '%s' has no compiled handler (not implemented).", app_id);
        return ESP_ERR_NOT_SUPPORTED;
    }

    ESP_LOGI(TAG, "Dispatching in-firmware app: %s (%s)", entry.app_name, app_id);
    fn();
    return ESP_OK;
}

esp_err_t app_registry_ensure_boot_os(void)
{
    /* Prefer ota_0 (the Atlas OS slot); fall back to factory if no ota_0. */
    const esp_partition_t *target = esp_partition_find_first(
        ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!target) {
        target = esp_partition_find_first(
            ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    }
    if (!target) {
        ESP_LOGE(TAG, "ensure_boot_os: no ota_0 or factory partition found!");
        return ESP_ERR_NOT_FOUND;
    }

    const esp_partition_t *boot_part = esp_ota_get_boot_partition();
    if (boot_part != target) {
        ESP_LOGI(TAG, "Resetting boot target to Atlas OS (%s).", target->label);
        return esp_ota_set_boot_partition(target);
    }
    return ESP_OK;
}

/* ── Firmware update (separate path, explicit user action only) ──────────── */

esp_err_t atlas_fw_update_from_sd(const char *bin_path,
                                  void (*progress_cb)(size_t written, size_t total))
{
    if (!bin_path || strlen(bin_path) == 0) return ESP_ERR_INVALID_ARG;

    /* 1. Open file */
    FILE *f = fopen(bin_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "fw_update: cannot open %s", bin_path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    size_t total_size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);

    if (total_size == 0) {
        fclose(f);
        ESP_LOGE(TAG, "fw_update: file is empty");
        return ESP_ERR_INVALID_SIZE;
    }

    /* 2. Locate the inactive OTA partition */
    const esp_partition_t *update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        fclose(f);
        ESP_LOGE(TAG, "fw_update: no OTA update partition available");
        return ESP_ERR_NOT_FOUND;
    }

    if (total_size > update_part->size) {
        fclose(f);
        ESP_LOGE(TAG, "fw_update: file %u bytes > partition %lu bytes",
                 (unsigned int)total_size, (unsigned long)update_part->size);
        return ESP_ERR_INVALID_SIZE;
    }

    /* 3. Quick sanity-check image magic before touching flash.
     * Full image validation (hash, secure-boot signature) is performed
     * internally by esp_ota_end(); no need to call esp_image_verify() here. */
    uint8_t hdr_buf[8];
    size_t hdr_read = fread(hdr_buf, 1, sizeof(hdr_buf), f);
    fseek(f, 0, SEEK_SET);

    if (hdr_read < 8 || hdr_buf[0] != 0xE9) {
        fclose(f);
        ESP_LOGE(TAG, "fw_update: invalid ESP image magic (0x%02X, expected 0xE9)", hdr_buf[0]);
        return ESP_ERR_INVALID_ARG;
    }

    ESP_LOGI(TAG, "fw_update: writing %u bytes to partition '%s'...",
             (unsigned int)total_size, update_part->label);

    /* 4. Begin OTA session */
    esp_ota_handle_t ota_handle;
    esp_err_t err = esp_ota_begin(update_part, total_size, &ota_handle);
    if (err != ESP_OK) {
        fclose(f);
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 5. Stream file to flash */
    uint8_t buf[4096];
    size_t bytes_written = 0;
    while (bytes_written < total_size) {
        size_t to_read = sizeof(buf);
        if (bytes_written + to_read > total_size) to_read = total_size - bytes_written;
        size_t n = fread(buf, 1, to_read, f);
        if (n == 0) break;

        err = esp_ota_write(ota_handle, buf, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            fclose(f);
            return err;
        }
        bytes_written += n;
        if (progress_cb) progress_cb(bytes_written, total_size);
    }
    fclose(f);

    /* 6. Finalise (includes image validation) */
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s (image invalid?)", esp_err_to_name(err));
        return err;
    }

    /* 7. Commit boot target and reboot */
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Firmware update complete. Rebooting into %s...", update_part->label);
    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK; /* unreachable */
}
