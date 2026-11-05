/**
 * @file atlas_captive.c
 * @brief SoftAP & Captive Portal Implementation for ATLAS OS
 */
#include "atlas_captive.h"
#include "atlas_log.h"
#include "atlas_wifi.h"

#include <string.h>
#include <stdio.h>
#include <sys/param.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static httpd_handle_t           s_http_server = NULL;
static esp_netif_t             *s_ap_netif = NULL;
static TaskHandle_t             s_dns_task_handle = NULL;
static int                      s_dns_sock = -1;
static bool                     s_running = false;
static atlas_captive_cred_cb_t  s_cred_callback = NULL;

/* ── Embedded Retro Mobile HTML Setup Portal ─────────────────────────────── */
static const char s_portal_html[] =
"<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>"
"<title>ATLAS Wi-Fi Setup</title><style>"
"body{background:#0a0c10;color:#00ffcc;font-family:monospace;margin:0;padding:24px;text-align:center}"
".box{max-width:380px;margin:0 auto;background:#141822;border:2px solid #00ffcc;border-radius:8px;padding:20px;box-shadow:0 0 16px rgba(0,255,204,0.2)}"
"h1{color:#ffb000;font-size:22px;margin-bottom:8px;letter-spacing:1px}"
"p{color:#8899aa;font-size:13px;line-height:1.4}"
"label{display:block;text-align:left;color:#00ffcc;font-size:12px;margin:14px 0 4px}"
"input{width:100%;box-sizing:border-box;background:#0d1117;border:1px solid #00ffcc;color:#ffffff;padding:12px;border-radius:4px;font-family:monospace;font-size:14px}"
"input:focus{outline:none;border-color:#ffb000;box-shadow:0 0 8px rgba(255,176,0,0.4)}"
"button{width:100%;background:#00ffcc;color:#0a0c10;border:none;padding:14px;font-weight:bold;font-size:15px;border-radius:4px;margin-top:20px;cursor:pointer;font-family:monospace}"
"button:hover{background:#ffb000}"
".status{margin-top:16px;color:#ffb000;font-size:13px;display:none}"
"</style></head><body><div class='box'>"
"<h1>ATLAS OS</h1><p>Handheld Terminal Setup</p>"
"<form id='f' action='/api/connect' method='POST'>"
"<label>WI-FI NETWORK (SSID):</label><input type='text' name='ssid' id='s' required placeholder='Enter network name'>"
"<label>WI-FI PASSWORD:</label><input type='password' name='pass' id='p' placeholder='Enter password (leave blank if open)'>"
"<button type='submit' id='b'>CONNECT ATLAS</button>"
"<div class='status' id='st'>Sending credentials to Atlas...</div>"
"</form></div>"
"<script>"
"document.getElementById('f').onsubmit=function(e){"
"document.getElementById('b').disabled=true;"
"document.getElementById('st').style.display='block';"
"};"
"</script></body></html>";

/* ── Lightweight DNS Captive Redirection Server ──────────────────────────── */
static void dns_server_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t buffer[512];
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_dns_sock < 0) {
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    while (s_running) {
        int len = recvfrom(s_dns_sock, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len > 12) {
            /* DNS Header: Flags -> 0x8180 (Standard response, no error) */
            buffer[2] = 0x81;
            buffer[3] = 0x80;
            buffer[6] = 0x00; /* Answer count = 1 */
            buffer[7] = 0x01;

            /* Parse Query section */
            int q_idx = 12;
            while (q_idx < len && buffer[q_idx] != 0) {
                q_idx += buffer[q_idx] + 1;
            }
            q_idx += 5; /* Skip null byte + Type(2) + Class(2) */

            if (q_idx + 16 <= (int)sizeof(buffer)) {
                /* Append Answer: Pointer to domain name */
                buffer[q_idx++] = 0xc0;
                buffer[q_idx++] = 0x0c;
                /* Type A */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x01;
                /* Class IN */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x01;
                /* TTL: 60 seconds */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x3c;
                /* Data Length: 4 bytes (IPv4) */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x04;
                /* IP: 192.168.4.1 */
                buffer[q_idx++] = 192;
                buffer[q_idx++] = 168;
                buffer[q_idx++] = 4;
                buffer[q_idx++] = 1;

                sendto(s_dns_sock, buffer, q_idx, 0,
                       (struct sockaddr *)&client_addr, client_addr_len);
            }
        }
    }

    close(s_dns_sock);
    s_dns_sock = -1;
    vTaskDelete(NULL);
}

/* ── HTTP Request Handlers ───────────────────────────────────────────────── */
static esp_err_t portal_root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, s_portal_html, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_connect_post_handler(httpd_req_t *req)
{
    char buf[256];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};

    char *token = strtok(buf, "&");
    while (token) {
        if (strncmp(token, "ssid=", 5) == 0) {
            snprintf(ssid, sizeof(ssid), "%s", token + 5);
        } else if (strncmp(token, "pass=", 5) == 0) {
            snprintf(pass, sizeof(pass), "%s", token + 5);
        }
        token = strtok(NULL, "&");
    }

    /* URL Decode simple + to space and %20 */
    for (size_t i = 0; i < strlen(ssid); i++) {
        if (ssid[i] == '+') ssid[i] = ' ';
    }
    for (size_t i = 0; i < strlen(pass); i++) {
        if (pass[i] == '+') pass[i] = ' ';
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Captive portal received credentials for SSID: '%s'", ssid);

    if (s_cred_callback && strlen(ssid) > 0) {
        s_cred_callback(ssid, pass);
    }

    const char *resp = "<!DOCTYPE html><html><body style='background:#0a0c10;color:#00ffcc;font-family:monospace;text-align:center;padding:40px'>"
                       "<h2>CREDENTIALS SAVED!</h2><p>Atlas is now connecting to your Wi-Fi...</p>"
                       "<p style='color:#ffb000'>You can close this window and look at your Atlas screen.</p></body></html>";
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

/* Captive portal redirect for Apple and Android checks */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_send(req, NULL, 0);
    return ESP_OK;
}

esp_err_t atlas_captive_start(atlas_captive_cred_cb_t cred_cb, char *out_ssid, size_t max_ssid_len)
{
    if (s_running) return ESP_OK;

    s_cred_callback = cred_cb;

    esp_err_t ret = esp_netif_init();
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) return ret;

    if (!s_ap_netif) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
    }

    /* Generate unique SSID using MAC address */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char ssid[32];
    snprintf(ssid, sizeof(ssid), "ATLAS-SETUP-%02X%02X", mac[4], mac[5]);
    if (out_ssid && max_ssid_len > 0) {
        snprintf(out_ssid, max_ssid_len, "%s", ssid);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);

    wifi_config_t ap_cfg = {
        .ap = {
            .channel = 1,
            .max_connection = 4,
            .authmode = WIFI_AUTH_OPEN,
        }
    };
    snprintf((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), "%s", ssid);
    ap_cfg.ap.ssid_len = strlen(ssid);

    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    esp_wifi_start();

    s_running = true;

    /* Start DNS Redirect Task */
    xTaskCreate(dns_server_task, "dns_task", 3072, NULL, 5, &s_dns_task_handle);

    /* Start HTTP Server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.stack_size = 4096;

    if (httpd_start(&s_http_server, &http_cfg) == ESP_OK) {
        httpd_uri_t root_uri = {
            .uri = "/",
            .method = HTTP_GET,
            .handler = portal_root_get_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &root_uri);

        httpd_uri_t post_uri = {
            .uri = "/api/connect",
            .method = HTTP_POST,
            .handler = portal_connect_post_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &post_uri);

        /* Apple captive portal detection endpoint */
        httpd_uri_t apple_uri = {
            .uri = "/hotspot-detect.html",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &apple_uri);

        /* Android captive portal detection endpoint */
        httpd_uri_t google_uri = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &google_uri);
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "SoftAP Captive Portal started on SSID: '%s' (IP: 192.168.4.1)", ssid);
    return ESP_OK;
}

esp_err_t atlas_captive_stop(void)
{
    if (!s_running) return ESP_OK;

    s_running = false;

    if (s_http_server) {
        httpd_stop(s_http_server);
        s_http_server = NULL;
    }

    if (s_dns_sock >= 0) {
        close(s_dns_sock);
        s_dns_sock = -1;
    }

    esp_wifi_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "SoftAP Captive Portal stopped.");
    return ESP_OK;
}

bool atlas_captive_is_running(void)
{
    return s_running;
}
