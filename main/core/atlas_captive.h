/**
 * @file atlas_captive.h
 * @brief SoftAP & Captive Portal Web Provisioning Engine for ATLAS OS
 */
#pragma once

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*atlas_captive_cred_cb_t)(const char *ssid, const char *password);

/**
 * @brief Start SoftAP, Captive DNS daemon, and embedded HTTP setup portal.
 *
 * @param cred_cb Callback fired when the user submits Wi-Fi credentials from their smartphone.
 * @param out_ssid Buffer to receive the generated SoftAP SSID (e.g. "ATLAS-SETUP-A1B2").
 * @param max_ssid_len Size of out_ssid buffer.
 * @return ESP_OK on success.
 */
esp_err_t atlas_captive_start(atlas_captive_cred_cb_t cred_cb, char *out_ssid, size_t max_ssid_len);

/**
 * @brief Stop SoftAP, DNS server, and HTTP server.
 */
esp_err_t atlas_captive_stop(void);

/**
 * @brief Check if captive portal is actively running.
 */
bool atlas_captive_is_running(void);

#ifdef __cplusplus
}
#endif

