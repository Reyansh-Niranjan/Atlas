/**
 * @file app_registry.c
 * @brief NVS Application Registry & Fast Deletion Engine
 */
#include "app_registry.h"

#include <string.h>
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "app_registry";
#define NVS_NAMESPACE       "atlas_apps"
#define NVS_KEY_APP_COUNT   "app_cnt"

static SemaphoreHandle_t s_reg_mutex = NULL;

/* ── Default System Applications ─────────────────────────────────────────── */
static const atlas_app_entry_t s_default_apps[] = {
    {
        .app_id       = "pkg_store",
        .app_name     = "APKG Store",
        .tagline      = "Installer & OTA Hub",
        .version      = "1.0.0",
        .flash_offset = 0x6A0000, /* ota_1 partition start */
        .size         = 0x400000,
        .is_installed = true,
    },
    {
        .app_id       = "settings",
        .app_name     = "Settings",
        .tagline      = "Telemetry & Cal",
        .version      = "1.0.0",
        .flash_offset = 0x000000,
        .size         = 0,
        .is_installed = true,
    },
    {
        .app_id       = "novaslate",
        .app_name     = "NovaSlate",
        .tagline      = "Curriculum & Reader",
        .version      = "1.0.0",
        .flash_offset = 0x000000,
        .size         = 0,
        .is_installed = true,
    },
    {
        .app_id       = "music",
        .app_name     = "Music",
        .tagline      = "I2S Audio Player",
        .version      = "1.0.0",
        .flash_offset = 0x000000,
        .size         = 0,
        .is_installed = true,
    }
};

/* ── Recursive Directory Cleaner Helper ──────────────────────────────────── */
static void delete_directory_recursive(const char *path)
{
    DIR *d = opendir(path);
    if (!d) return;

    struct dirent *dir;
    char filepath[512];

    while ((dir = readdir(d)) != NULL) {
        if (strcmp(dir->d_name, ".") == 0 || strcmp(dir->d_name, "..") == 0) {
            continue;
        }
        int len = snprintf(filepath, sizeof(filepath), "%s/%s", path, dir->d_name);
        if (len > 0 && len < (int)sizeof(filepath)) {
            struct stat st;
            if (stat(filepath, &st) == 0) {
                if (S_ISDIR(st.st_mode)) {
                    delete_directory_recursive(filepath);
                } else {
                    unlink(filepath);
                }
            }
        }
    }
    closedir(d);
    rmdir(path);
}

/* ── Public API Implementation ───────────────────────────────────────────── */

esp_err_t app_registry_init(void)
{
    if (!s_reg_mutex) {
        s_reg_mutex = xSemaphoreCreateMutex();
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
        ESP_LOGI(TAG, "Empty NVS app table. Seeding 4 default system applications...");
        size_t def_count = sizeof(s_default_apps) / sizeof(s_default_apps[0]);
        for (size_t i = 0; i < def_count; i++) {
            char key[16];
            snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
            nvs_set_blob(handle, key, &s_default_apps[i], sizeof(atlas_app_entry_t));
        }
        nvs_set_u32(handle, NVS_KEY_APP_COUNT, def_count);
        nvs_commit(handle);
        ESP_LOGI(TAG, "Seeded %u default applications to NVS.", (unsigned int)def_count);
    } else {
        ESP_LOGI(TAG, "Loaded app registry from NVS (%lu registered apps).", (unsigned long)count);
    }

    nvs_close(handle);
    return ESP_OK;
}

esp_err_t app_registry_get_all(atlas_app_entry_t *out_entries, size_t max_entries, size_t *out_count)
{
    if (!out_entries || max_entries == 0) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, NVS_KEY_APP_COUNT, &count);

    size_t loaded = 0;
    for (uint32_t i = 0; i < count && loaded < max_entries; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        size_t required_size = sizeof(atlas_app_entry_t);
        if (nvs_get_blob(handle, key, &out_entries[loaded], &required_size) == ESP_OK) {
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
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_reg_mutex);
        return err;
    }

    uint32_t count = 0;
    nvs_get_u32(handle, NVS_KEY_APP_COUNT, &count);

    bool found = false;
    for (uint32_t i = 0; i < count; i++) {
        char key[16];
        snprintf(key, sizeof(key), "app_%u", (unsigned int)i);
        atlas_app_entry_t entry;
        size_t req = sizeof(entry);
        if (nvs_get_blob(handle, key, &entry, &req) == ESP_OK) {
            if (strncmp(entry.app_id, app_id, sizeof(entry.app_id)) == 0) {
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
    nvs_set_blob(handle, key, entry, sizeof(atlas_app_entry_t));
    nvs_commit(handle);

    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);

    ESP_LOGI(TAG, "Application registered/updated: %s (%s)", entry->app_name, entry->app_id);
    return ESP_OK;
}

esp_err_t app_registry_uninstall(const char *app_id)
{
    if (!app_id) return ESP_ERR_INVALID_ARG;
    if (xSemaphoreTake(s_reg_mutex, portMAX_DELAY) != pdTRUE) return ESP_ERR_TIMEOUT;

    ESP_LOGI(TAG, "Initiating fast ghost-uninstall for '%s'...", app_id);

    /* 1. Fast removal of SD card assets at /sdcard/apps/<app_id> */
    char sd_app_dir[128];
    snprintf(sd_app_dir, sizeof(sd_app_dir), "/sdcard/apps/%s", app_id);
    delete_directory_recursive(sd_app_dir);
    ESP_LOGI(TAG, "Cleared assets at %s", sd_app_dir);

    /* 2. Update is_installed flag in NVS without physical flash sector erase */
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
                ESP_LOGI(TAG, "Marked '%s' uninstalled in NVS (ghost deletion complete).", app_id);
                break;
            }
        }
    }

    nvs_close(handle);
    xSemaphoreGive(s_reg_mutex);

    return updated ? ESP_OK : ESP_ERR_NOT_FOUND;
}
