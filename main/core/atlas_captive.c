/**
 * @file atlas_captive.c
 * @brief SoftAP & Captive Portal Implementation for ATLAS OS
 */
#include "atlas_captive.h"
#include "atlas_log.h"
#include "atlas_wifi.h"
#include "atlas_onboarding.h"

#include <string.h>
#include <errno.h>
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
"<label>GOOGLE GEMINI API KEY (OPTIONAL):</label><input type='text' name='gemini' id='g' placeholder='AIzaSy... (for Gemini Live app)'>"
"<button type='submit' id='b'>CONNECT ATLAS</button>"
"<div class='status' id='st'>Sending credentials to Atlas...</div>"
"</form></div>"
"<script>"
"document.getElementById('f').onsubmit=function(e){"
"document.getElementById('b').disabled=true;"
"document.getElementById('st').style.display='block';"
"};"
"</script></body></html>";

/* ── RFC 1035 Compliant DNS Captive Redirection Server ───────────────────── */
static void dns_server_task(void *pvParameters)
{
    (void)pvParameters;
    uint8_t buffer[512];

    s_dns_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (s_dns_sock < 0) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Failed to create DNS socket: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    /* Set socket timeout to 1 second so stopping doesn't block */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(s_dns_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in server_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY)
    };

    if (bind(s_dns_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ATLAS_LOG_ERROR(ATLAS_LOG_TAG_CORE, "Failed to bind DNS socket to port 53: errno %d", errno);
        close(s_dns_sock);
        s_dns_sock = -1;
        vTaskDelete(NULL);
        return;
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Captive DNS Redirection Server active on UDP port 53.");

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);

        int len = recvfrom(s_dns_sock, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 12) {
            continue;
        }

        /* Check opcode == 0 (standard query, bits 11-14 of flags) */
        if ((buffer[2] & 0x78) != 0) {
            continue;
        }

        /* DNS Header Flags: QR=1 (Response), RA=1 (Recursion Available), RCODE=0 (No Error) */
        buffer[2] |= 0x80;
        buffer[3] = 0x80;

        /* Parse Query Domain Name */
        int q_idx = 12;
        while (q_idx < len && buffer[q_idx] != 0) {
            q_idx += buffer[q_idx] + 1;
        }
        q_idx++; /* Skip null terminator */

        if (q_idx + 4 > len) {
            continue;
        }

        uint16_t qtype = ((uint16_t)buffer[q_idx] << 8) | buffer[q_idx + 1];
        uint16_t qclass = ((uint16_t)buffer[q_idx + 2] << 8) | buffer[q_idx + 3];
        q_idx += 4; /* Skip QTYPE and QCLASS */

        if (qtype == 0x0001 && qclass == 0x0001) {
            /* Type A (IPv4) IN Query: Provide 192.168.4.1 answer */
            buffer[6] = 0x00; /* ANCOUNT = 1 */
            buffer[7] = 0x01;

            if (q_idx + 16 <= (int)sizeof(buffer)) {
                /* Compressed name pointer to QNAME at offset 12 (0xc00c) */
                buffer[q_idx++] = 0xc0;
                buffer[q_idx++] = 0x0c;
                /* Type A */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x01;
                /* Class IN */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x01;
                /* TTL: 60s */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x3c;
                /* RDLENGTH: 4 bytes */
                buffer[q_idx++] = 0x00;
                buffer[q_idx++] = 0x04;
                /* RDATA: 192.168.4.1 */
                buffer[q_idx++] = 192;
                buffer[q_idx++] = 168;
                buffer[q_idx++] = 4;
                buffer[q_idx++] = 1;

                sendto(s_dns_sock, buffer, q_idx, 0,
                       (struct sockaddr *)&client_addr, client_addr_len);
            }
        } else {
            /* For Type AAAA (IPv6) or HTTPS (65): Return NODATA (0 answers, NOERROR) */
            buffer[6] = 0x00; /* ANCOUNT = 0 */
            buffer[7] = 0x00;
            sendto(s_dns_sock, buffer, q_idx, 0,
                   (struct sockaddr *)&client_addr, client_addr_len);
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
    char buf[512];
    int ret = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (ret <= 0) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }
    buf[ret] = '\0';

    char ssid[64] = {0};
    char pass[64] = {0};
    char gemini[128] = {0};

    char *token = strtok(buf, "&");
    while (token) {
        if (strncmp(token, "ssid=", 5) == 0) {
            snprintf(ssid, sizeof(ssid), "%s", token + 5);
        } else if (strncmp(token, "pass=", 5) == 0) {
            snprintf(pass, sizeof(pass), "%s", token + 5);
        } else if (strncmp(token, "gemini=", 7) == 0) {
            snprintf(gemini, sizeof(gemini), "%s", token + 7);
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
    for (size_t i = 0; i < strlen(gemini); i++) {
        if (gemini[i] == '+') gemini[i] = ' ';
    }

    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Captive portal received credentials for SSID: '%s'", ssid);

    if (strlen(gemini) > 0) {
        atlas_onboarding_write_gemini_key(gemini);
    }

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

/* Captive portal redirect for Apple, Android, Windows, and catch-all */
static esp_err_t portal_redirect_handler(httpd_req_t *req)
{
    ATLAS_LOG_INFO(ATLAS_LOG_TAG_CORE, "Captive portal redirecting '%s' to http://192.168.4.1/", req->uri);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_type(req, "text/html");
    const char *resp = "<!DOCTYPE html><html><head><meta http-equiv='refresh' content='0;url=http://192.168.4.1/'></head>"
                       "<body>Redirecting to <a href='http://192.168.4.1/'>ATLAS Setup</a></body></html>";
    httpd_resp_send(req, resp, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t portal_404_handler(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    return portal_redirect_handler(req);
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

    /* Configure DHCP Option 114 (RFC 8908 / RFC 8910 Captive Portal Identification) */
    if (s_ap_netif) {
        static const char s_portal_url[] = "http://192.168.4.1/";
        esp_netif_dhcps_stop(s_ap_netif);
        esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                               (void *)s_portal_url, strlen(s_portal_url));
        esp_netif_dhcps_start(s_ap_netif);
    }

    /* Start DNS Redirect Task with 4KB stack */
    xTaskCreate(dns_server_task, "dns_task", 4096, NULL, 5, &s_dns_task_handle);

    /* Start HTTP Server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 16;
    http_cfg.max_open_sockets = 13;
    http_cfg.lru_purge_enable = true;
    http_cfg.stack_size = 4096;

    if (httpd_start(&s_http_server, &http_cfg) == ESP_OK) {
        httpd_register_err_handler(s_http_server, HTTPD_404_NOT_FOUND, portal_404_handler);
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

        /* Android / Chrome captive portal detection endpoints */
        httpd_uri_t google_uri = {
            .uri = "/generate_204",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &google_uri);

        httpd_uri_t gen204_uri = {
            .uri = "/gen_204",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &gen204_uri);

        /* Windows NCSI & connecttest endpoints */
        httpd_uri_t win_uri1 = {
            .uri = "/connecttest.txt",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &win_uri1);

        httpd_uri_t win_uri2 = {
            .uri = "/ncsi.txt",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &win_uri2);

        /* Linux / Firefox detection endpoint */
        httpd_uri_t linux_uri = {
            .uri = "/success.txt",
            .method = HTTP_GET,
            .handler = portal_redirect_handler,
            .user_ctx = NULL
        };
        httpd_register_uri_handler(s_http_server, &linux_uri);
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
