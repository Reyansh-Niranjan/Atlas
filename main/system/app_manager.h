/**
 * @file app_manager.h
 * @brief Dynamic Application Manager for ATLAS OS
 *
 * Single source of truth for the application catalogue:
 * - Registers genuine built-in applications.
 * - Discovers installed applications from /sdcard/apps/installed/.
 * - Validates manifests and checks entrypoint integrity.
 * - Prevents duplicate IDs and path traversal.
 * - Manages application launch requests safely.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_APP_MAX_COUNT          16
#define ATLAS_APP_ID_MAX_LEN         32
#define ATLAS_APP_NAME_MAX_LEN       32
#define ATLAS_APP_VERSION_MAX_LEN    16
#define ATLAS_APP_TAGLINE_MAX_LEN    48
#define ATLAS_APP_ENTRYPOINT_MAX_LEN 64
#define ATLAS_APP_PATH_MAX_LEN       128

typedef void (*atlas_app_handler_t)(void);

typedef enum {
    ATLAS_APP_TYPE_BUILTIN = 0,
    ATLAS_APP_TYPE_INSTALLED
} atlas_app_type_t;

typedef enum {
    ATLAS_RUNTIME_BUILTIN = 0,
    ATLAS_RUNTIME_WASM,
    ATLAS_RUNTIME_NATIVE,
    ATLAS_RUNTIME_UNKNOWN
} atlas_runtime_t;

typedef struct {
    char                app_id[ATLAS_APP_ID_MAX_LEN];
    char                name[ATLAS_APP_NAME_MAX_LEN];
    char                version[ATLAS_APP_VERSION_MAX_LEN];
    char                tagline[ATLAS_APP_TAGLINE_MAX_LEN];
    char                entrypoint[ATLAS_APP_ENTRYPOINT_MAX_LEN];
    char                app_dir[ATLAS_APP_PATH_MAX_LEN];
    char                icon_path[ATLAS_APP_PATH_MAX_LEN];
    atlas_app_type_t    type;
    atlas_runtime_t     runtime;
    char                runtime_str[16];
    bool                is_installed;
    bool                is_launchable;
    atlas_app_handler_t launch_fn;
} atlas_app_t;

/**
 * @brief Initialize the application manager and register built-in applications.
 */
esp_err_t app_manager_init(void);

/**
 * @brief Scan /sdcard/apps/installed/ for valid application bundles.
 * Safe to call if SD card is unmounted or empty.
 */
esp_err_t app_manager_scan_installed(void);

/**
 * @brief Get total number of registered applications (built-in + installed).
 */
size_t app_manager_get_count(void);

/**
 * @brief Retrieve application metadata by catalogue index.
 */
esp_err_t app_manager_get_at(size_t index, atlas_app_t *out_app);

/**
 * @brief Retrieve application metadata by stable unique application ID.
 */
esp_err_t app_manager_get_by_id(const char *app_id, atlas_app_t *out_app);

/**
 * @brief Dispatch an application launch request.
 *
 * @return ESP_OK              - Handler executed and returned.
 *         ESP_ERR_NOT_SUPPORTED - Application runtime not supported (cannot launch).
 *         ESP_ERR_NOT_FOUND    - Application not found in catalogue.
 */
esp_err_t app_manager_launch(const char *app_id);

/**
 * @brief Ensure boot partition points to Atlas OS image.
 */
esp_err_t app_manager_ensure_boot_os(void);

#ifdef __cplusplus
}
#endif

