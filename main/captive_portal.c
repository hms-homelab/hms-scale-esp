#include "captive_portal.h"
#include "nvs_config.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_mac.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "portal";

/* ── DNS hijack server ── */

static TaskHandle_t s_dns_task = NULL;

static void dns_server_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "DNS socket failed: %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int yes = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons(53),
        .sin_addr.s_addr = inet_addr("192.168.4.1"),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "DNS bind failed: %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t portal_ip[4] = {192, 168, 4, 1};
    uint8_t buf[512];
    struct sockaddr_in client;
    socklen_t clen = sizeof(client);

    ESP_LOGI(TAG, "DNS hijack server running");

    while (true) {
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client, &clen);
        if (len < 12) continue;

        /* Build minimal DNS response: copy query, set response flags, add A record */
        buf[2]  = 0x81; buf[3]  = 0x80;   /* flags: response, no error */
        buf[4]  = 0x00; buf[5]  = 0x01;   /* QDCOUNT = 1 */
        buf[6]  = 0x00; buf[7]  = 0x01;   /* ANCOUNT = 1 */
        buf[8]  = 0x00; buf[9]  = 0x00;   /* NSCOUNT = 0 */
        buf[10] = 0x00; buf[11] = 0x00;   /* ARCOUNT = 0 */

        /* Skip past the question section */
        int pos = 12;
        while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
        pos += 5; /* null terminator + QTYPE(2) + QCLASS(2) */

        if (pos + 16 > (int)sizeof(buf)) continue;

        /* Answer: pointer to name, type A, class IN, TTL 0, data = portal IP */
        buf[pos++] = 0xC0; buf[pos++] = 0x0C;
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        buf[pos++] = 0x00; buf[pos++] = 0x01;
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x00;
        buf[pos++] = 0x00; buf[pos++] = 0x04;
        buf[pos++] = portal_ip[0];
        buf[pos++] = portal_ip[1];
        buf[pos++] = portal_ip[2];
        buf[pos++] = portal_ip[3];

        sendto(sock, buf, pos, 0, (struct sockaddr *)&client, clen);
    }

    close(sock);
    vTaskDelete(NULL);
}

/* ── HTML ── */

static const char PORTAL_HTML[] =
    "<!DOCTYPE html><html><head><meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width,initial-scale=1'>"
    "<title>Giraffe Scale Setup</title>"
    "<style>"
    "body{background:#0F1120;color:#E0E0E0;font-family:system-ui;margin:0;padding:20px;}"
    ".card{background:#1A1D35;border-radius:12px;padding:24px;max-width:380px;margin:40px auto;}"
    "h1{color:#667EEA;font-size:20px;margin:0 0 4px;}"
    "h2{color:#888;font-size:12px;margin:0 0 20px;font-weight:normal;}"
    "label{display:block;color:#888;font-size:12px;margin:12px 0 4px;}"
    "select,input[type=text],input[type=password]{width:100%;padding:10px;background:#252840;"
    "border:1px solid #333;border-radius:8px;color:#E0E0E0;font-size:14px;box-sizing:border-box;}"
    "button{width:100%;padding:12px;background:#667EEA;color:#fff;border:none;border-radius:8px;"
    "font-size:15px;font-weight:600;cursor:pointer;margin-top:20px;}"
    "button:disabled{background:#444;}"
    ".status{color:#4ADE80;font-size:13px;margin-top:12px;text-align:center;}"
    "</style></head><body>"
    "<div class='card'>"
    "<h1>Giraffe Scale Setup</h1>"
    "<h2>WiFi &amp; Webhook Configuration</h2>"
    "<form id='f' method='POST' action='/save'>"
    "<label>WiFi Network</label>"
    "<select name='ssid' id='ssid'><option>Scanning...</option></select>"
    "<label>WiFi Password</label>"
    "<input type='password' name='pass' id='pass' placeholder='WiFi password'>"
    "<label>Server (IP:Port)</label>"
    "<input type='text' name='server_addr' id='server_addr' "
    "value='" CONFIG_DEFAULT_SERVER "'>"
    "<button type='submit' id='btn'>Save &amp; Connect</button>"
    "</form>"
    "<div class='status' id='st'></div>"
    "</div>"
    "<script>"
    "fetch('/scan').then(r=>r.json()).then(d=>{"
    "let s=document.getElementById('ssid');s.innerHTML='';"
    "d.forEach(n=>{let o=document.createElement('option');o.value=n;o.textContent=n;s.appendChild(o);});"
    "}).catch(()=>{document.getElementById('st').textContent='Scan failed';});"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();let b=document.getElementById('btn');b.disabled=true;b.textContent='Saving...';"
    "let body='ssid='+encodeURIComponent(document.getElementById('ssid').value)"
    "+'&pass='+encodeURIComponent(document.getElementById('pass').value)"
    "+'&server_addr='+encodeURIComponent(document.getElementById('server_addr').value);"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:body}).then(r=>r.text()).then(t=>{document.getElementById('st').textContent=t;});"
    "};"
    "</script></body></html>";

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

static esp_err_t handle_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t handle_redirect(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static esp_err_t handle_scan(httpd_req_t *req)
{
    uint16_t num = 0;
    esp_wifi_scan_start(NULL, true);
    esp_wifi_scan_get_ap_num(&num);
    if (num > 20) num = 20;

    wifi_ap_record_t *aps = calloc(num, sizeof(wifi_ap_record_t));
    esp_wifi_scan_get_ap_records(&num, aps);

    char buf[1024] = "[";
    size_t pos = 1;
    for (int i = 0; i < num; i++) {
        if (i > 0) buf[pos++] = ',';
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\"%s\"", (char *)aps[i].ssid);
        if (pos >= sizeof(buf) - 10) break;
    }
    buf[pos++] = ']';
    buf[pos] = 0;
    free(aps);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, pos);
}

static esp_err_t handle_save(httpd_req_t *req)
{
    char body[768] = {0};
    int len = httpd_req_recv(req, body, sizeof(body) - 1);
    if (len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "No data");
        return ESP_FAIL;
    }
    body[len] = 0;

    char ssid[33] = {0}, pass[65] = {0}, server_addr[256] = {0};

    parse_field(body, "ssid", ssid, sizeof(ssid));
    parse_field(body, "pass", pass, sizeof(pass));
    parse_field(body, "server_addr", server_addr, sizeof(server_addr));

    if (strlen(ssid) == 0) {
        return httpd_resp_send(req, "Please select a network", HTTPD_RESP_USE_STRLEN);
    }

    /* Save WiFi credentials */
    nvs_config_set_wifi(ssid, pass);
    ESP_LOGI(TAG, "WiFi saved: %s", ssid);

    /* Save webhook URL if provided */
    if (strlen(server_addr) > 0) {
        nvs_config_set_server(server_addr);
        ESP_LOGI(TAG, "Server (IP:Port) saved: %s", server_addr);
    }

    httpd_resp_send(req, "Saved! Rebooting...", HTTPD_RESP_USE_STRLEN);

    if (s_dns_task) {
        vTaskDelete(s_dns_task);
        s_dns_task = NULL;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

/* ── Public API ── */

void captive_portal_start(void)
{
    /* Build AP SSID with last 4 chars of MAC */
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char mac_suffix[5];
    snprintf(mac_suffix, sizeof(mac_suffix), "%02X%02X", mac[4], mac[5]);

    char ap_ssid[32];
    snprintf(ap_ssid, sizeof(ap_ssid), "GiraffeScale-%s", mac_suffix);

    ESP_LOGI(TAG, "Starting captive portal AP: %s", ap_ssid);

    esp_netif_init();
    esp_event_loop_create_default();
    esp_netif_create_default_wifi_ap();
    /* Also create STA netif so APSTA scan works */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 2;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Start DNS hijack */
    xTaskCreate(dns_server_task, "dns_hijack", 4096, NULL, 5, &s_dns_task);

    /* Start HTTP server */
    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.max_uri_handlers = 8;
    http_cfg.uri_match_fn = httpd_uri_match_wildcard;

    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &http_cfg));

    httpd_uri_t uri_root  = { .uri = "/",     .method = HTTP_GET,  .handler = handle_root };
    httpd_uri_t uri_scan  = { .uri = "/scan",  .method = HTTP_GET,  .handler = handle_scan };
    httpd_uri_t uri_save  = { .uri = "/save",  .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t uri_catch = { .uri = "/*",     .method = HTTP_GET,  .handler = handle_redirect };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_catch);

    ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1/");

    /* Block forever — reboot happens in handle_save */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
