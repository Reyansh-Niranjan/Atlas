/**
 * @file app_registry.c
 * @brief NVS Application Registry, In-Firmware Dispatch, and Firmware Update Engine
 *
 * See app_registry.h for the architecture rationale.
 */
#include "app_registry.h"
#include "app_manager.h"
#include "atlas_ota.h"
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
    /* Delegate to the active Application Manager */
    return app_manager_uninstall(app_id);
}

esp_err_t app_registry_scan_sdcard(void)
{
    /* SD application scanning is now owned by app_manager via manifests in /sdcard/apps/installed/ */
    ESP_LOGI(TAG, "Legacy SD scan: delegating to app_manager_scan_installed().");
    return app_manager_scan_installed();
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
    return app_manager_launch(app_id);
}

esp_err_t app_registry_ensure_boot_os(void)
{
    return atlas_ota_validate_boot();
}

/* ── Firmware update (delegates to atlas_ota engine) ────────────────────── */

esp_err_t atlas_fw_update_from_sd(const char *bin_path,
                                  void (*progress_cb)(size_t written, size_t total))
{
    return atlas_ota_update_from_file(bin_path, progress_cb);
}
