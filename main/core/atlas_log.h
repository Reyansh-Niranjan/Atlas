/**
 * @file atlas_log.h
 * @brief Centralized Logging Subsystem for ATLAS OS
 *
 * Wraps ESP-IDF logging facilities with standardized component prefixes,
 * severity filtering, and unified formatting across the OS.
 */
#pragma once

#include "esp_log.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Standard ATLAS OS Component Tags */
#define ATLAS_LOG_TAG_SYS     "ATLAS_SYS"
#define ATLAS_LOG_TAG_BOOT    "ATLAS_BOOT"
#define ATLAS_LOG_TAG_CORE    "ATLAS_CORE"
#define ATLAS_LOG_TAG_DRV     "ATLAS_DRV"
#define ATLAS_LOG_TAG_FS      "ATLAS_FS"
#define ATLAS_LOG_TAG_APP     "ATLAS_APP"
#define ATLAS_LOG_TAG_UI      "ATLAS_UI"

/* Standard Log Macros with Component Identification */
#define ATLAS_LOG_ERROR(tag, format, ...)  ESP_LOGE(tag, format, ##__VA_ARGS__)
#define ATLAS_LOG_WARN(tag, format, ...)   ESP_LOGW(tag, format, ##__VA_ARGS__)
#define ATLAS_LOG_INFO(tag, format, ...)   ESP_LOGI(tag, format, ##__VA_ARGS__)
#define ATLAS_LOG_DEBUG(tag, format, ...)  ESP_LOGD(tag, format, ##__VA_ARGS__)

#ifdef __cplusplus
}
#endif

