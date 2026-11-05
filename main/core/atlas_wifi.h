/**
 * @file atlas_wifi.h
 * @brief High-reliability Wi-Fi Station Manager for ATLAS OS
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ATLAS_WIFI_SSID_MAX_LEN     33
#define ATLAS_WIFI_PASS_MAX_LEN     65

typedef enum {
    ATLAS_WIFI_DISCONNECTED = 0,
    ATLAS_WIFI_CONNECTING,
    ATLAS_WIFI_CONNECTED,
    ATLAS_WIFI_FAILED
} atlas_wifi_status_t;

/**
 * @brief Initialize the ATLAS Wi-Fi subsystem (Netif, Event handlers).
 * Safe to call multiple times (idempotent).
 */
esp_err_t atlas_wifi_init(void);

/**
 * @brief Connect to a Wi-Fi network in Station mode with a synchronous timeout.
 * Automatically saves valid credentials to NVS under "atlas_wifi" namespace.
 *
 * @param ssid       SSID string (up to 32 chars).
 * @param password   WPA2 passphrase (up to 64 chars, or empty/NULL for open).
 * @param timeout_ms Timeout in milliseconds to wait for IP assignment.
 * @return ESP_OK on connection & DHCP IP; ESP_ERR_TIMEOUT, ESP_FAIL on auth error.
 */
esp_err_t atlas_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms);

/**
 * @brief Disconnect and stop Wi-Fi station.
 */
esp_err_t atlas_wifi_disconnect(void);

/**
 * @brief Check if currently connected and has acquired DHCP IP.
 */
bool atlas_wifi_is_connected(void);

/**
 * @brief Get current Wi-Fi status.
 */
atlas_wifi_status_t atlas_wifi_get_status(void);

/**
 * @brief Get current RSSI signal strength in dBm (-30 to -100). Returns 0 if disconnected.
 */
int8_t atlas_wifi_get_rssi(void);

/**
 * @brief Get assigned IPv4 address string (e.g. "192.168.1.150").
 */
esp_err_t atlas_wifi_get_ip_str(char *out_ip, size_t max_len);

/**
 * @brief Load saved Wi-Fi credentials from NVS.
 */
esp_err_t atlas_wifi_load_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len);

/**
 * @brief Save Wi-Fi credentials to NVS.
 */
esp_err_t atlas_wifi_save_credentials(const char *ssid, const char *password);

/**
 * @brief Get last disconnect reason code as a human-readable diagnosis string.
 */
const char *atlas_wifi_get_last_error_desc(void);

#ifdef __cplusplus
}
#endif
