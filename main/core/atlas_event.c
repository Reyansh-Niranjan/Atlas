/**
 * @file atlas_event.c
 * @brief Inter-Service Event Bus Implementation for ATLAS OS
 */
#include "atlas_event.h"
#include "atlas_log.h"
#include <string.h>

ESP_EVENT_DEFINE_BASE(ATLAS_EVENT_BASE);

static bool s_event_loop_initialized = false;

esp_err_t atlas_event_init(void)
{
    if (s_event_loop_initialized) {
        return ESP_OK;
    }

    esp_err_t ret = esp_event_loop_create_default();
    if (ret == ESP_ERR_INVALID_STATE) {
        /* Default event loop already created by another subsystem */
        s_event_loop_initialized = true;
        return ESP_OK;
    }
    if (ret != ESP_OK) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Failed to create default event loop: %s", esp_err_to_name(ret));
        return ret;
    }

    s_event_loop_initialized = true;
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "ATLAS Event Bus initialized successfully.");
    return ESP_OK;
}

esp_err_t atlas_event_post(int32_t event_id, const void *event_data, size_t event_data_size, TickType_t ticks_to_wait)
{
    if (!s_event_loop_initialized) {
        esp_err_t err = atlas_event_init();
        if (err != ESP_OK) return err;
    }

    return esp_event_post(ATLAS_EVENT_BASE, event_id, event_data, event_data_size, ticks_to_wait);
}

esp_err_t atlas_event_register_handler(int32_t event_id, esp_event_handler_t handler, void *handler_args)
{
    if (!s_event_loop_initialized) {
        esp_err_t err = atlas_event_init();
        if (err != ESP_OK) return err;
    }

    return esp_event_handler_instance_register(ATLAS_EVENT_BASE, event_id, handler, handler_args, NULL);
}

esp_err_t atlas_event_unregister_handler(int32_t event_id, esp_event_handler_t handler)
{
    if (!s_event_loop_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    return esp_event_handler_unregister(ATLAS_EVENT_BASE, event_id, handler);
}

