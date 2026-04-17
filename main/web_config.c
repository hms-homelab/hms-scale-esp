#include "web_config.h"
#include "nvs_config.h"
#include "http_webhook.h"
#include "scale_ble_client.h"
#include "esp_http_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "web_cfg";

/* Cached state for the status page */
static char s_wifi_ssid[33] = {0};
static char s_webhook_url[256] = {0};
static char s_last_weight[16] = "N/A";
static char s_last_time[32] = "N/A";

void web_config_set_last_measurement(float weight_kg)
{
    snprintf(s_last_weight, sizeof(s_last_weight), "%.2f kg", weight_kg);
    snprintf(s_last_time, sizeof(s_last_time), "just now");
}

/* ── URL decode helper ── */

static size_t url_decode(const char *src, char *dst, size_t dst_size)
{
    size_t j = 0;
    for (size_t i = 0; src[i] && j < dst_size - 1; i++) {
        if (src[i] == '+') {
            dst[j++] = ' ';
        } else if (src[i] == '%' && src[i + 1] && src[i + 2]) {
            char hex[3] = {src[i + 1], src[i + 2], 0};
            dst[j++] = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            dst[j++] = src[i];
        }
    }
    dst[j] = '\0';
    return j;
}

/* ── Form field parser ── */

static bool parse_field(char *body, const char *name, char *out, size_t out_size)
{
    out[0] = '\0';
    size_t nlen = strlen(name);

    char *pos = body;
    while ((pos = strstr(pos, name)) != NULL) {
        if (pos != body && *(pos - 1) != '&') { pos += nlen; continue; }
        if (pos[nlen] != '=') { pos += nlen; continue; }
        break;
    }
    if (!pos) return false;

    char *val = pos + nlen + 1;
    char *end = strchr(val, '&');
    char saved = 0;
    if (end) { saved = *end; *end = '\0'; }

    url_decode(val, out, out_size);

    if (end) *end = saved;
    return true;
}

/* ── HTTP handlers ── */

static esp_err_t handle_status(httpd_req_t *req)
{
    /* Get IP address */
    char ip_str[16] = "N/A";
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif) {
        esp_netif_ip_info_t ip_info;
        if (esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
            snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        }
    }

    bool ble_connected = scale_ble_is_connected();

    char html[4096];
    snprintf(html, sizeof(html),
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>Giraffe Scale</title>"
        "<style>"
        "body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
        ".card{background:#1A1D35;border-radius:12px;padding:24px;max-width:380px;margin:40px auto;}"
        "h1{color:#667EEA;font-size:20px;margin:0 0 16px;}"
        ".row{display:flex;justify-content:space-between;padding:8px 0;border-bottom:1px solid #252840;}"
        ".lbl{color:#888;font-size:13px;}.val{color:#E0E0E0;font-size:13px;}"
        ".on{color:#4ADE80;}.off{color:#EF4444;}"
        "label{display:block;color:#888;font-size:12px;margin:16px 0 4px;}"
        "input[type=text]{width:100%%;padding:10px;background:#252840;border:1px solid #333;"
        "border-radius:8px;color:#E0E0E0;font-size:14px;box-sizing:border-box;}"
        "button{padding:10px 16px;border:none;border-radius:8px;font-size:14px;"
        "font-weight:600;cursor:pointer;margin-top:12px;}"
        ".btn-save{background:#667EEA;color:#fff;width:100%%;}"
        ".btn-reset{background:#EF4444;color:#fff;width:100%%;margin-top:8px;}"
        ".status{color:#4ADE80;font-size:13px;margin-top:8px;text-align:center;}"
        "</style></head><body>"
        "<div class='card'>"
        "<h1>Giraffe Scale</h1>"
        "<div class='row'><span class='lbl'>WiFi SSID</span><span class='val'>%s</span></div>"
        "<div class='row'><span class='lbl'>IP Address</span><span class='val'>%s</span></div>"
        "<div class='row'><span class='lbl'>BLE Scale</span>"
        "<span class='val %s'>%s</span></div>"
        "<div class='row'><span class='lbl'>Last Weight</span><span class='val'>%s</span></div>"
        "<div class='row'><span class='lbl'>Webhook URL</span>"
        "<span class='val' style='word-break:break-all;font-size:11px;'>%s</span></div>"
        "<form method='POST' action='/config'>"
        "<label>Update Webhook URL</label>"
        "<input type='text' name='webhook_url' value='%s'>"
        "<button type='submit' class='btn-save'>Update Webhook</button>"
        "</form>"
        "<form method='POST' action='/reset'>"
        "<button type='submit' class='btn-reset' "
        "onclick=\"return confirm('Reset WiFi and reboot?')\">Reset WiFi</button>"
        "</form>"
        "<div class='status' id='st'></div>"
        "</div></body></html>",
        s_wifi_ssid,
        ip_str,
        ble_connected ? "on" : "off",
        ble_connected ? "Connected" : "Scanning...",
        s_last_weight,
        s_webhook_url,
        s_webhook_url);

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_config(httpd_req_t *req)
{
    char body[512] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[len] = 0;

    char webhook_url[256] = {0};
    parse_field(body, "webhook_url", webhook_url, sizeof(webhook_url));

    if (strlen(webhook_url) > 0) {
        nvs_config_set_webhook_url(webhook_url);
        webhook_set_url(webhook_url);
        strncpy(s_webhook_url, webhook_url, sizeof(s_webhook_url) - 1);
        ESP_LOGI(TAG, "Webhook URL updated: %s", webhook_url);
    }

    /* Redirect back to status page */
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_reset(httpd_req_t *req)
{
    ESP_LOGW(TAG, "WiFi reset requested via web UI");
    nvs_config_clear();

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req,
        "<!DOCTYPE html><html><head><meta charset='utf-8'>"
        "<style>body{background:#0F1120;color:#E0E0E0;font-family:system-ui;"
        "display:flex;justify-content:center;align-items:center;height:100vh;margin:0;}"
        "</style></head><body><p>WiFi cleared. Rebooting to captive portal...</p></body></html>",
        HTTPD_RESP_USE_STRLEN);

    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── Public API ── */

esp_err_t web_config_start(void)
{
    /* Cache current config for the status page */
    nvs_config_get_wifi(s_wifi_ssid, sizeof(s_wifi_ssid), (char[65]){0}, 65);
    if (!nvs_config_get_webhook_url(s_webhook_url, sizeof(s_webhook_url))) {
        strncpy(s_webhook_url, CONFIG_DEFAULT_WEBHOOK_URL, sizeof(s_webhook_url) - 1);
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 6;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start config server: %s", esp_err_to_name(err));
        return err;
    }

    httpd_uri_t uri_status = { .uri = "/",       .method = HTTP_GET,  .handler = handle_status };
    httpd_uri_t uri_config = { .uri = "/config",  .method = HTTP_POST, .handler = handle_config };
    httpd_uri_t uri_reset  = { .uri = "/reset",   .method = HTTP_POST, .handler = handle_reset };

    httpd_register_uri_handler(server, &uri_status);
    httpd_register_uri_handler(server, &uri_config);
    httpd_register_uri_handler(server, &uri_reset);

    ESP_LOGI(TAG, "Config web server started on port 80");
    return ESP_OK;
}
