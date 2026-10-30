/**
 * @file atlas_service.c
 * @brief Lightweight Service Registry & Lifecycle Manager Implementation
 */
#include "atlas_service.h"
#include "atlas_event.h"
#include "atlas_log.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static atlas_service_desc_t s_services[ATLAS_SERVICE_MAX_COUNT];
static size_t               s_service_count = 0;
static SemaphoreHandle_t    s_service_mutex = NULL;

esp_err_t atlas_service_registry_init(void)
{
    if (s_service_mutex != NULL) {
        return ESP_OK;
    }

    s_service_mutex = xSemaphoreCreateMutex();
    if (!s_service_mutex) {
        return ESP_ERR_NO_MEM;
    }

    memset(s_services, 0, sizeof(s_services));
    s_service_count = 0;
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Service registry initialized.");
    return ESP_OK;
}

esp_err_t atlas_service_register(const char *id,
                                const char *name,
                                atlas_service_fn_t init_fn,
                                atlas_service_fn_t start_fn,
                                atlas_service_fn_t stop_fn)
{
    if (!id || strlen(id) == 0) return ESP_ERR_INVALID_ARG;
    if (atlas_service_registry_init() != ESP_OK) return ESP_ERR_NO_MEM;

    if (xSemaphoreTake(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    /* Check for duplicate registration */
    for (size_t i = 0; i < s_service_count; i++) {
        if (strncmp(s_services[i].id, id, sizeof(s_services[i].id)) == 0) {
            xSemaphoreGive(s_service_mutex);
            ATLAS_LOG_WARN(ATLAS_LOG_TAG_CORE, "Service '%s' already registered.", id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    if (s_service_count >= ATLAS_SERVICE_MAX_COUNT) {
        xSemaphoreGive(s_service_mutex);
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Service registry full, cannot register '%s'.", id);
        return ESP_ERR_NO_MEM;
    }

    atlas_service_desc_t *desc = &s_services[s_service_count++];
    strncpy(desc->id, id, sizeof(desc->id) - 1);
    strncpy(desc->name, name ? name : id, sizeof(desc->name) - 1);
    desc->state      = ATLAS_SERVICE_STATE_REGISTERED;
    desc->last_error = ESP_OK;
    desc->init_fn    = init_fn;
    desc->start_fn   = start_fn;
    desc->stop_fn    = stop_fn;

    xSemaphoreGive(s_service_mutex);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Registered service: [%s] '%s'", id, desc->name);

    atlas_event_service_t evt = { .service_id = id, .error_code = ESP_OK };
    atlas_event_post(ATLAS_EVENT_SERVICE_REGISTERED, &evt, sizeof(evt), 0);

    return ESP_OK;
}

esp_err_t atlas_service_start(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    if (!s_service_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    atlas_service_desc_t *target = NULL;
    for (size_t i = 0; i < s_service_count; i++) {
        if (strcmp(s_services[i].id, id) == 0) {
            target = &s_services[i];
            break;
        }
    }

    if (!target) {
        xSemaphoreGive(s_service_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (target->state == ATLAS_SERVICE_STATE_RUNNING) {
        xSemaphoreGive(s_service_mutex);
        return ESP_OK;
    }

    /* Initialize if needed */
    if (target->init_fn && target->state == ATLAS_SERVICE_STATE_REGISTERED) {
        target->state = ATLAS_SERVICE_STATE_INITIALIZING;
        esp_err_t err = target->init_fn();
        if (err != ESP_OK) {
            target->state = ATLAS_SERVICE_STATE_FAILED;
            target->last_error = err;
            xSemaphoreGive(s_service_mutex);
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Service '%s' init failed: %s", id, esp_err_to_name(err));
            atlas_event_service_t evt = { .service_id = id, .error_code = err };
            atlas_event_post(ATLAS_EVENT_SERVICE_FAILED, &evt, sizeof(evt), 0);
            return err;
        }
    }

    /* Start */
    if (target->start_fn) {
        esp_err_t err = target->start_fn();
        if (err != ESP_OK) {
            target->state = ATLAS_SERVICE_STATE_FAILED;
            target->last_error = err;
            xSemaphoreGive(s_service_mutex);
            ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Service '%s' start failed: %s", id, esp_err_to_name(err));
            atlas_event_service_t evt = { .service_id = id, .error_code = err };
            atlas_event_post(ATLAS_EVENT_SERVICE_FAILED, &evt, sizeof(evt), 0);
            return err;
        }
    }

    target->state = ATLAS_SERVICE_STATE_RUNNING;
    target->last_error = ESP_OK;
    xSemaphoreGive(s_service_mutex);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Service '%s' started successfully.", id);
    atlas_event_service_t evt = { .service_id = id, .error_code = ESP_OK };
    atlas_event_post(ATLAS_EVENT_SERVICE_STARTED, &evt, sizeof(evt), 0);

    return ESP_OK;
}

esp_err_t atlas_service_stop(const char *id)
{
    if (!id) return ESP_ERR_INVALID_ARG;
    if (!s_service_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    atlas_service_desc_t *target = NULL;
    for (size_t i = 0; i < s_service_count; i++) {
        if (strcmp(s_services[i].id, id) == 0) {
            target = &s_services[i];
            break;
        }
    }

    if (!target) {
        xSemaphoreGive(s_service_mutex);
        return ESP_ERR_NOT_FOUND;
    }

    if (target->state != ATLAS_SERVICE_STATE_RUNNING) {
        xSemaphoreGive(s_service_mutex);
        return ESP_OK;
    }

    if (target->stop_fn) {
        esp_err_t err = target->stop_fn();
        if (err != ESP_OK) {
            target->last_error = err;
            xSemaphoreGive(s_service_mutex);
            return err;
        }
    }

    target->state = ATLAS_SERVICE_STATE_STOPPED;
    xSemaphoreGive(s_service_mutex);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Service '%s' stopped.", id);
    atlas_event_service_t evt = { .service_id = id, .error_code = ESP_OK };
    atlas_event_post(ATLAS_EVENT_SERVICE_STOPPED, &evt, sizeof(evt), 0);

    return ESP_OK;
}

esp_err_t atlas_service_get_state(const char *id, atlas_service_state_t *out_state, esp_err_t *out_last_err)
{
    if (!id || !out_state) return ESP_ERR_INVALID_ARG;
    if (!s_service_mutex) return ESP_ERR_INVALID_STATE;

    if (xSemaphoreTake(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    for (size_t i = 0; i < s_service_count; i++) {
        if (strcmp(s_services[i].id, id) == 0) {
            *out_state = s_services[i].state;
            if (out_last_err) *out_last_err = s_services[i].last_error;
            xSemaphoreGive(s_service_mutex);
            return ESP_OK;
        }
    }

    xSemaphoreGive(s_service_mutex);
    return ESP_ERR_NOT_FOUND;
}

size_t atlas_service_get_all(atlas_service_desc_t *out_services, size_t max_count)
{
    if (!out_services || max_count == 0 || !s_service_mutex) return 0;

    if (xSemaphoreTake(s_service_mutex, portMAX_DELAY) != pdTRUE) {
        return 0;
    }

    size_t count = (s_service_count < max_count) ? s_service_count : max_count;
    memcpy(out_services, s_services, count * sizeof(atlas_service_desc_t));
    xSemaphoreGive(s_service_mutex);

    return count;
}

