/**
 * @file atlas_wifi.c
 * @brief High-reliability Wi-Fi Station Manager Implementation for ATLAS OS
 */
#include "atlas_wifi.h"
#include "atlas_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#define NVS_NAMESPACE_WIFI       "atlas_wifi"
#define NVS_KEY_SSID             "ssid"
#define NVS_KEY_PASS             "pass"

#define WIFI_CONNECTED_BIT       BIT0
#define WIFI_FAIL_BIT            BIT1

static EventGroupHandle_t s_wifi_event_group = NULL;
static esp_netif_t       *s_sta_netif = NULL;
static bool               s_initialized = false;
static atlas_wifi_status_t s_status = ATLAS_WIFI_DISCONNECTED;
static uint8_t            s_last_disconnect_reason = 0;
static char               s_current_ip[16] = {0};

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Wi-Fi Station connected to AP. Awaiting DHCP lease...");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *)event_data;
        s_last_disconnect_reason = disc ? disc->reason : 0;
        s_status = ATLAS_WIFI_DISCONNECTED;
        s_current_ip[0] = '\0';
        ATLAS_LOG_WARN(ATLAS_LOG_TAG_CORE, "Wi-Fi disconnected. Reason code: %d (%s)",
                       s_last_disconnect_reason, atlas_wifi_get_last_error_desc());
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_current_ip, sizeof(s_current_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_status = ATLAS_WIFI_CONNECTED;
        s_last_disconnect_reason = 0;
        ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Wi-Fi link ready. Assigned IP: %s", s_current_ip);
        if (s_wifi_event_group) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

esp_err_t atlas_wifi_init(void)
{
    if (s_initialized) return ESP_OK;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    if (!s_sta_netif) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ret = esp_wifi_init(&cfg);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) {
        return ret;
    }

    s_wifi_event_group = xEventGroupCreate();

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, &instance_any_id);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, &instance_got_ip);

    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);

    s_initialized = true;
    return ESP_OK;
}

esp_err_t atlas_wifi_connect(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!ssid || strlen(ssid) == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t ret = atlas_wifi_init();
    if (ret != ESP_OK) return ret;

    wifi_config_t wifi_cfg = {0};
    snprintf((char *)wifi_cfg.sta.ssid, sizeof(wifi_cfg.sta.ssid), "%s", ssid);
    if (password && strlen(password) > 0) {
        snprintf((char *)wifi_cfg.sta.password, sizeof(wifi_cfg.sta.password), "%s", password);
    }
    wifi_cfg.sta.threshold.authmode = (password && strlen(password) > 0) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    wifi_cfg.sta.pmf_cfg.capable = true;
    wifi_cfg.sta.pmf_cfg.required = false;

    if (s_wifi_event_group) {
        xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }

    s_status = ATLAS_WIFI_CONNECTING;
    s_last_disconnect_reason = 0;

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_wifi_set_config(WIFI_IF_STA, &wifi_cfg);
    esp_wifi_start();

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Connecting to Wi-Fi SSID '%s' (timeout %lu ms)...",
                   ssid, (unsigned long)timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                          WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                          pdFALSE, pdFALSE,
                                          pdMS_TO_TICKS(timeout_ms));

    if (bits & WIFI_CONNECTED_BIT) {
        atlas_wifi_save_credentials(ssid, password);
        return ESP_OK;
    }

    s_status = ATLAS_WIFI_FAILED;
    return (bits & WIFI_FAIL_BIT) ? ESP_FAIL : ESP_ERR_TIMEOUT;
}

esp_err_t atlas_wifi_disconnect(void)
{
    if (!s_initialized) return ESP_OK;
    s_status = ATLAS_WIFI_DISCONNECTED;
    s_current_ip[0] = '\0';
    return esp_wifi_disconnect();
}

bool atlas_wifi_is_connected(void)
{
    return (s_status == ATLAS_WIFI_CONNECTED);
}

atlas_wifi_status_t atlas_wifi_get_status(void)
{
    return s_status;
}

int8_t atlas_wifi_get_rssi(void)
{
    if (s_status != ATLAS_WIFI_CONNECTED) return 0;
    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        return ap_info.rssi;
    }
    return 0;
}

esp_err_t atlas_wifi_get_ip_str(char *out_ip, size_t max_len)
{
    if (!out_ip || max_len == 0) return ESP_ERR_INVALID_ARG;
    if (s_status != ATLAS_WIFI_CONNECTED || s_current_ip[0] == '\0') {
        out_ip[0] = '\0';
        return ESP_ERR_INVALID_STATE;
    }
    snprintf(out_ip, max_len, "%s", s_current_ip);
    return ESP_OK;
}

esp_err_t atlas_wifi_load_credentials(char *out_ssid, size_t ssid_len, char *out_pass, size_t pass_len)
{
    if (!out_ssid || ssid_len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READONLY, &nvs);
    if (err != ESP_OK) return err;

    size_t s_len = ssid_len;
    err = nvs_get_str(nvs, NVS_KEY_SSID, out_ssid, &s_len);
    if (err == ESP_OK && out_pass && pass_len > 0) {
        size_t p_len = pass_len;
        if (nvs_get_str(nvs, NVS_KEY_PASS, out_pass, &p_len) != ESP_OK) {
            out_pass[0] = '\0';
        }
    }
    nvs_close(nvs);
    return err;
}

esp_err_t atlas_wifi_save_credentials(const char *ssid, const char *password)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t nvs;
    esp_err_t err = nvs_open(NVS_NAMESPACE_WIFI, NVS_READWRITE, &nvs);
    if (err != ESP_OK) return err;

    nvs_set_str(nvs, NVS_KEY_SSID, ssid);
    if (password && strlen(password) > 0) {
        nvs_set_str(nvs, NVS_KEY_PASS, password);
    } else {
        nvs_erase_key(nvs, NVS_KEY_PASS);
    }
    nvs_commit(nvs);
    nvs_close(nvs);
    return ESP_OK;
}

const char *atlas_wifi_get_last_error_desc(void)
{
    switch (s_last_disconnect_reason) {
        case 201: /* WIFI_REASON_NO_AP_FOUND */
            return "Network SSID not found in range. Check name.";
        case 202: /* WIFI_REASON_AUTH_FAIL */
        case 203: /* WIFI_REASON_AUTH_EXPIRE */
        case 204: /* WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT */
        case 205: /* WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT */
            return "Incorrect Wi-Fi password or auth failed.";
        case 200: /* WIFI_REASON_BEACON_TIMEOUT */
            return "Signal lost or router out of range.";
        case 206: /* WIFI_REASON_IE_IN_4WAY_DIFFERS */
        case 207: /* WIFI_REASON_GROUP_CIPHER_INVALID */
        case 208: /* WIFI_REASON_PAIRWISE_CIPHER_INVALID */
            return "Unsupported Wi-Fi encryption type.";
        case 0:
            return "No error recorded.";
        default:
            return "Connection timed out or router rejected link.";
    }
}
