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
#define NVS_KEY_STAGED_APP  "staged_app"

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
            out_entries[loaded].is_staged = app_registry_is_staged(out_entries[loaded].app_id);
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
                entry.is_staged = app_registry_is_staged(app_id);
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

    /* 1. Remove binary from SD card if present */
    char bin_path[128];
    snprintf(bin_path, sizeof(bin_path), "/sdcard/apps/%s.bin", app_id);
    unlink(bin_path);

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

bool app_registry_is_staged(const char *app_id)
{
    if (!app_id) return false;
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    char staged[32] = {0};
    size_t len = sizeof(staged);
    esp_err_t err = nvs_get_str(handle, NVS_KEY_STAGED_APP, staged, &len);
    nvs_close(handle);
    if (err == ESP_OK) {
        return (strncmp(staged, app_id, sizeof(staged)) == 0);
    }
    return false;
}

esp_err_t app_registry_scan_sdcard(void)
{
    DIR *dir = opendir("/sdcard/apps");
    if (!dir) {
        mkdir("/sdcard/apps", 0777);
        dir = opendir("/sdcard/apps");
        if (!dir) return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Scanning /sdcard/apps/ for executable application binaries...");
    struct dirent *entry;
    int found_count = 0;

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        size_t len = strlen(entry->d_name);
        if (len > 4 && strcmp(entry->d_name + len - 4, ".bin") == 0) {
            char app_id[32] = {0};
            char app_name[32] = {0};
            size_t name_len = len - 4;
            if (name_len >= sizeof(app_id)) name_len = sizeof(app_id) - 1;
            memcpy(app_id, entry->d_name, name_len);
            app_id[name_len] = '\0';

            /* Format display name */
            snprintf(app_name, sizeof(app_name), "%s", app_id);
            if (app_name[0] >= 'a' && app_name[0] <= 'z') {
                app_name[0] -= ('a' - 'A');
            }

            char full_path[128];
            snprintf(full_path, sizeof(full_path), "/sdcard/apps/%s", entry->d_name);

            struct stat st;
            uint32_t fsize = 0;
            if (stat(full_path, &st) == 0) {
                fsize = (uint32_t)st.st_size;
            }

            atlas_app_entry_t app = {0};
            snprintf(app.app_id, sizeof(app.app_id), "%s", app_id);
            snprintf(app.app_name, sizeof(app.app_name), "%s", app_name);
            snprintf(app.tagline, sizeof(app.tagline), "SD Card Binary (%lu KB)", (unsigned long)(fsize / 1024));
            snprintf(app.version, sizeof(app.version), "1.0.0");
            snprintf(app.bin_path, sizeof(app.bin_path), "%s", full_path);
            app.size = fsize;
            app.flash_offset = 0x6A0000;
            app.is_installed = true;
            app.is_staged = app_registry_is_staged(app_id);

            app_registry_register(&app);
            found_count++;
            ESP_LOGI(TAG, "Discovered SD App: %s -> %s (Staged: %s)", app.app_name, full_path, app.is_staged ? "YES" : "NO");
        }
    }
    closedir(dir);
    ESP_LOGI(TAG, "SD scan complete: %d binaries indexed.", found_count);
    return ESP_OK;
}

esp_err_t app_registry_stage_and_launch(const char *app_id, void (*progress_cb)(size_t written, size_t total))
{
    if (!app_id) return ESP_ERR_INVALID_ARG;

    atlas_app_entry_t entry;
    esp_err_t err = app_registry_get(app_id, &entry);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Cannot launch app '%s': not registered", app_id);
        return err;
    }

    const esp_partition_t *ota_1 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_1, NULL);
    if (!ota_1) {
        ESP_LOGE(TAG, "Execution partition ota_1 not found in partition table!");
        return ESP_ERR_NOT_FOUND;
    }

    /* 1. Fast path: Check if already staged in ota_1 flash slot */
    if (app_registry_is_staged(app_id)) {
        ESP_LOGI(TAG, "=========================================================");
        ESP_LOGI(TAG, " FAST BOOT: '%s' is ALREADY staged in ota_1 partition!  ", app_id);
        ESP_LOGI(TAG, " Instant 0-second launch without flash re-write!        ");
        ESP_LOGI(TAG, "=========================================================");
        err = esp_ota_set_boot_partition(ota_1);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to set boot partition: %s", esp_err_to_name(err));
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return ESP_OK;
    }

    /* 2. Staging path: Flash binary from SD card to ota_1 */
    char file_path[128];
    if (strlen(entry.bin_path) > 0) {
        snprintf(file_path, sizeof(file_path), "%s", entry.bin_path);
    } else {
        snprintf(file_path, sizeof(file_path), "/sdcard/apps/%s.bin", app_id);
    }

    FILE *f = fopen(file_path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "Failed to open app binary at %s", file_path);
        return ESP_ERR_NOT_FOUND;
    }

    fseek(f, 0, SEEK_END);
    size_t total_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (total_size == 0 || total_size > ota_1->size) {
        ESP_LOGE(TAG, "Binary size invalid (%u bytes, ota_1 cap: %lu bytes)", (unsigned int)total_size, (unsigned long)ota_1->size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }

    ESP_LOGI(TAG, "Staging '%s' (%u bytes) from SD card into ota_1 partition...", app_id, (unsigned int)total_size);
    esp_ota_handle_t ota_handle;
    err = esp_ota_begin(ota_1, total_size, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        fclose(f);
        return err;
    }

    uint8_t buffer[4096];
    size_t bytes_written = 0;
    while (bytes_written < total_size) {
        size_t to_read = sizeof(buffer);
        if (bytes_written + to_read > total_size) {
            to_read = total_size - bytes_written;
        }
        size_t n = fread(buffer, 1, to_read, f);
        if (n == 0) break;

        err = esp_ota_write(ota_handle, buffer, n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            fclose(f);
            return err;
        }

        bytes_written += n;
        if (progress_cb) {
            progress_cb(bytes_written, total_size);
        }
    }
    fclose(f);

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        return err;
    }

    /* 3. Record staged app in NVS */
    nvs_handle_t nvs_h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_h) == ESP_OK) {
        nvs_set_str(nvs_h, NVS_KEY_STAGED_APP, app_id);
        nvs_commit(nvs_h);
        nvs_close(nvs_h);
    }

    ESP_LOGI(TAG, "Staging complete! Setting boot partition to ota_1 and booting '%s'...", app_id);
    err = esp_ota_set_boot_partition(ota_1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        return err;
    }

    vTaskDelay(pdMS_TO_TICKS(150));
    esp_restart();
    return ESP_OK;
}

esp_err_t app_registry_ensure_boot_os(void)
{
    const esp_partition_t *ota_0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_OTA_0, NULL);
    if (!ota_0) {
        ota_0 = esp_partition_find_first(ESP_PARTITION_TYPE_APP, ESP_PARTITION_SUBTYPE_APP_FACTORY, NULL);
    }
    if (!ota_0) return ESP_ERR_NOT_FOUND;

    const esp_partition_t *boot_part = esp_ota_get_boot_partition();
    if (boot_part != ota_0) {
        ESP_LOGI(TAG, "Ensuring default boot target is Atlas OS (ota_0)...");
        return esp_ota_set_boot_partition(ota_0);
    }
    return ESP_OK;
}
