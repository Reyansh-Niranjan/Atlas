/**
 * @file atlas_service.h
 * @brief Lightweight Service Registry & Lifecycle Manager for ATLAS OS
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_SERVICE_MAX_COUNT   16
#define ATLAS_SERVICE_ID_MAX_LEN  32
#define ATLAS_SERVICE_NAME_MAX_LEN 48

typedef enum {
    ATLAS_SERVICE_STATE_UNREGISTERED = 0,
    ATLAS_SERVICE_STATE_REGISTERED,
    ATLAS_SERVICE_STATE_INITIALIZING,
    ATLAS_SERVICE_STATE_RUNNING,
    ATLAS_SERVICE_STATE_STOPPED,
    ATLAS_SERVICE_STATE_FAILED
} atlas_service_state_t;

typedef esp_err_t (*atlas_service_fn_t)(void);

typedef struct {
    char                 id[ATLAS_SERVICE_ID_MAX_LEN];
    char                 name[ATLAS_SERVICE_NAME_MAX_LEN];
    atlas_service_state_t state;
    esp_err_t            last_error;
    atlas_service_fn_t   init_fn;
    atlas_service_fn_t   start_fn;
    atlas_service_fn_t   stop_fn;
} atlas_service_desc_t;

/**
 * @brief Initialize service registry internal structures.
 */
esp_err_t atlas_service_registry_init(void);

/**
 * @brief Register a new service with the registry.
 */
esp_err_t atlas_service_register(const char *id,
                                const char *name,
                                atlas_service_fn_t init_fn,
                                atlas_service_fn_t start_fn,
                                atlas_service_fn_t stop_fn);

/**
 * @brief Start a registered service.
 */
esp_err_t atlas_service_start(const char *id);

/**
 * @brief Stop a running service.
 */
esp_err_t atlas_service_stop(const char *id);

/**
 * @brief Query current state and last error of a service.
 */
esp_err_t atlas_service_get_state(const char *id, atlas_service_state_t *out_state, esp_err_t *out_last_err);

/**
 * @brief Retrieve a snapshot of all registered services.
 */
size_t atlas_service_get_all(atlas_service_desc_t *out_services, size_t max_count);

#ifdef __cplusplus
}
#endif

