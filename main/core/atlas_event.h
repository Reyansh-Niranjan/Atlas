/**
 * @file atlas_event.h
 * @brief Inter-Service Event Bus for ATLAS OS
 *
 * Built directly on ESP-IDF's default event loop facility (esp_event).
 * Provides decoupled, thread-safe, queue-backed publish-subscribe messaging
 * without dynamic heap allocations for event descriptors.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "esp_event.h"

#ifdef __cplusplus
extern "C" {
#endif

ESP_EVENT_DECLARE_BASE(ATLAS_EVENT_BASE);

/**
 * @brief ATLAS OS Typed Event Identifiers
 */
typedef enum {
    /* System & Boot Events */
    ATLAS_EVENT_BOOT_STARTED = 0,
    ATLAS_EVENT_BOOT_STAGE_ADVANCE,
    ATLAS_EVENT_BOOT_COMPLETED,
    ATLAS_EVENT_SYSTEM_SHUTDOWN,

    /* Storage Events */
    ATLAS_EVENT_STORAGE_MOUNTED,
    ATLAS_EVENT_STORAGE_UNMOUNTED,
    ATLAS_EVENT_STORAGE_ERROR,

    /* Service Lifecycle Events */
    ATLAS_EVENT_SERVICE_REGISTERED,
    ATLAS_EVENT_SERVICE_STARTED,
    ATLAS_EVENT_SERVICE_STOPPED,
    ATLAS_EVENT_SERVICE_FAILED,

    /* Input & UI Events */
    ATLAS_EVENT_INPUT_STATE_CHANGED,

    /* Application Events */
    ATLAS_EVENT_APP_CATALOGUE_CHANGED,

    ATLAS_EVENT_MAX
} atlas_event_id_t;

/**
 * @brief Payload for ATLAS_EVENT_BOOT_STAGE_ADVANCE
 */
typedef struct {
    int32_t   stage_id;
    const char *stage_name;
    esp_err_t status;
} atlas_event_boot_stage_t;

/**
 * @brief Payload for ATLAS_EVENT_STORAGE_*
 */
typedef struct {
    const char *mount_point;
    uint64_t    total_bytes;
    uint64_t    free_bytes;
    bool        success;
} atlas_event_storage_t;

/**
 * @brief Payload for ATLAS_EVENT_SERVICE_*
 */
typedef struct {
    const char *service_id;
    esp_err_t   error_code;
} atlas_event_service_t;

/**
 * @brief Initialize the ATLAS OS event subsystem and default loop.
 * Safe to call multiple times.
 */
esp_err_t atlas_event_init(void);

/**
 * @brief Dispatch an event to all registered subscribers.
 * Payload is copied into the event loop queue (no manual free needed).
 *
 * @param event_id        Target event identifier.
 * @param event_data      Pointer to payload struct (or NULL if no payload).
 * @param event_data_size Size of payload struct in bytes.
 * @param ticks_to_wait   FreeRTOS ticks to wait if queue is congested.
 */
esp_err_t atlas_event_post(int32_t event_id, const void *event_data, size_t event_data_size, TickType_t ticks_to_wait);

/**
 * @brief Register an event handler for a specific event ID or ESP_EVENT_ANY_ID.
 */
esp_err_t atlas_event_register_handler(int32_t event_id, esp_event_handler_t handler, void *handler_args);

/**
 * @brief Unregister a previously registered event handler.
 */
esp_err_t atlas_event_unregister_handler(int32_t event_id, esp_event_handler_t handler);

#ifdef __cplusplus
}
#endif

