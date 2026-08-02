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
#include "freertos/event_groups.h"
#include <string.h>
#include <stdlib.h>

static const char *TAG = "portal";

/* ── Verify-join state machine (SDD-015, ported from cpapdash-push-c3) ──
 * IDLE -> (POST /save) -> JOINING -> JOIN_OK   persist + reboot
 *                                 -> JOIN_FAIL reason kept, back to accepting /save
 *
 * Credentials are held in RAM and written to NVS only after the join has
 * proved good, so NVS never holds creds that don't work. Before this, /save
 * wrote NVS and rebooted blind: a typo cost two reboots and the user landed
 * back on the portal with no explanation, because the auth-failure path in
 * main.c wiped the bad creds on the far side.
 * ---------------------------------------------------------------------- */

typedef enum {
    PORTAL_IDLE = 0,
    PORTAL_JOINING,
    PORTAL_JOIN_OK,
    PORTAL_JOIN_FAIL,
} portal_state_t;

static volatile portal_state_t s_state = PORTAL_IDLE;
static volatile uint8_t s_fail_reason = 0;   /* 1..4, valid in JOIN_FAIL */

/* The in-flight onboarding attempt. Held, not stored, until the join proves good. */
static struct {
    char ssid[33];
    char pass[65];
    char server[256];
} s_pending;

static EventGroupHandle_t s_join_eg = NULL;
static const int JOIN_OK_BIT   = BIT0;
static const int JOIN_FAIL_BIT = BIT1;
static volatile uint16_t s_last_disconnect_reason = 0;
static volatile int  s_join_retries = 0;
static volatile bool s_join_active  = false;  /* mute handler outside an attempt */
#define PORTAL_JOIN_TIMEOUT_MS 25000
#define PORTAL_JOIN_MAX_RETRY  3

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
    ".status{color:#E0E0E0;font-size:13px;margin-top:12px;text-align:center;}"
    ".ok{color:#4ADE80;}.err{color:#F87171;}"
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
    "var REASONS={1:'Network not found. If it is 5 GHz only, this board cannot join it.',"
    "2:'Wrong Wi-Fi password.',"
    "3:'Connected, but the router never gave it an IP address.',"
    "4:'Could not connect to that network.'};"
    /* Polling is bounded and starts only after a save. 40 x 2s comfortably
       outlasts the 25s join timeout plus its retries, and cannot spin forever
       if the board reboots out from under the page. */
    "var tries=0;"
    "function done(cls,msg,again){"
    "let s=document.getElementById('st'),b=document.getElementById('btn');"
    "s.className='status '+cls;s.textContent=msg;"
    "if(again){b.disabled=false;b.textContent='Save & Connect';}"
    "}"
    "function poll(){"
    "if(++tries>40){done('err','Timed out. Nothing was saved, try again.',1);return;}"
    "fetch('/info').then(r=>r.json()).then(function(d){"
    "if(d.state=='ok'){done('ok','Connected. Saving and rebooting...',0);return;}"
    "if(d.state=='fail'){done('err',(REASONS[d.reason]||'Could not connect.')+' Nothing was saved, try again.',1);return;}"
    "setTimeout(poll,2000);"
    /* The AP hops to the target network's channel during the join, so the
       phone can drop for a couple of seconds. Keep polling through it. */
    "}).catch(function(){setTimeout(poll,2000);});"
    "}"
    "document.getElementById('f').onsubmit=function(e){"
    "e.preventDefault();let b=document.getElementById('btn');b.disabled=true;b.textContent='Checking...';"
    "let s=document.getElementById('st');s.className='status';s.textContent='Checking Wi-Fi...';"
    "tries=0;"   /* reset, or a retry after a failure times out immediately */
    "let body='ssid='+encodeURIComponent(document.getElementById('ssid').value)"
    "+'&pass='+encodeURIComponent(document.getElementById('pass').value)"
    "+'&server_addr='+encodeURIComponent(document.getElementById('server_addr').value);"
    "fetch('/save',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},"
    "body:body}).then(function(){setTimeout(poll,1500);}).catch(function(){setTimeout(poll,1500);});"
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

/* ── Verify-join ── */

/* Collapse the esp WIFI_REASON_* space into something the portal can explain. */
static uint8_t map_wifi_reason(uint16_t reason)
{
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return 1;   /* SSID not found, often a 5 GHz-only network */
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return 2;   /* wrong password */
        default:
            return 4;   /* other / association failure */
    }
}

static void portal_wifi_event_handler(void *arg, esp_event_base_t base,
                                      int32_t id, void *data)
{
    if (!s_join_active) return;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *dis = (wifi_event_sta_disconnected_t *)data;
        if (dis) s_last_disconnect_reason = dis->reason;
        if (s_join_retries < PORTAL_JOIN_MAX_RETRY) {
            s_join_retries++;
            ESP_LOGI(TAG, "Join: disconnected (reason=%u), retry %d/%d",
                     dis ? dis->reason : 0, s_join_retries, PORTAL_JOIN_MAX_RETRY);
            esp_wifi_connect();
        } else {
            xEventGroupSetBits(s_join_eg, JOIN_FAIL_BIT);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_join_eg, JOIN_OK_BIT);
    }
}

/* Attempt the STA join with the AP still up (APSTA). Persist only on success.
 * Single radio: the AP hops to the target network's channel during the join,
 * so the phone may drop for a couple of seconds; the portal page polls /info
 * fault-tolerantly and rides it out.
 *
 * In-session retries are safe on this board: BLE is only brought up in
 * app_main step 7, after WiFi connects, and captive_portal_start() blocks
 * before that. So there is no WiFi + BLE coexistence heap pressure here, which
 * is the reason push-c3 originally rebooted on failure instead of retrying. */
static void portal_join_task(void *arg)
{
    ESP_LOGI(TAG, "Join: attempting '%s' with AP up (free heap=%u) [creds held, not yet stored]",
             s_pending.ssid, (unsigned)esp_get_free_heap_size());

    wifi_config_t sta_config = {0};
    strncpy((char *)sta_config.sta.ssid, s_pending.ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, s_pending.pass, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = s_pending.pass[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
    sta_config.sta.pmf_cfg.capable  = true;
    sta_config.sta.pmf_cfg.required = false;

    xEventGroupClearBits(s_join_eg, JOIN_OK_BIT | JOIN_FAIL_BIT);
    s_last_disconnect_reason = 0;
    s_join_retries = 0;
    s_join_active  = true;

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err == ESP_OK) err = esp_wifi_connect();

    EventBits_t bits = 0;
    if (err == ESP_OK) {
        bits = xEventGroupWaitBits(s_join_eg, JOIN_OK_BIT | JOIN_FAIL_BIT,
                                   pdTRUE, pdFALSE,
                                   pdMS_TO_TICKS(PORTAL_JOIN_TIMEOUT_MS));
    }
    s_join_active = false;

    if (bits & JOIN_OK_BIT) {
        /* Join proved good: NOW persist (store-after-join), then reboot into
         * normal operation. The grace period lets the page read the final
         * state at least twice across the AP channel hop. */
        nvs_config_set_wifi(s_pending.ssid, s_pending.pass);
        if (s_pending.server[0]) nvs_config_set_server(s_pending.server);
        s_state = PORTAL_JOIN_OK;
        ESP_LOGI(TAG, "Join: success, creds persisted; rebooting");
        vTaskDelay(pdMS_TO_TICKS(8000));
        if (s_dns_task) {
            vTaskDelete(s_dns_task);
            s_dns_task = NULL;
        }
        esp_restart();
    } else {
        /* Prefer the captured disconnect reason. Retries mean a real
         * NO_AP_FOUND or AUTH_FAIL surfaces here as a timeout, but the reason
         * was recorded on each attempt. Only fall back to 3 (associated but
         * DHCP never completed) when nothing was captured at all. */
        uint16_t raw = s_last_disconnect_reason;
        s_fail_reason = raw ? map_wifi_reason(raw) : 3;
        s_state = PORTAL_JOIN_FAIL;
        esp_wifi_disconnect();
        ESP_LOGW(TAG, "Join: failed (esp reason=%u -> portal %u), creds NOT stored; portal stays up",
                 (unsigned)raw, (unsigned)s_fail_reason);
    }
    vTaskDelete(NULL);
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
    if (num == 0) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_send(req, "[]", 2);
    }
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

    if (s_state == PORTAL_JOINING) {
        return httpd_resp_send(req, "Already connecting...", HTTPD_RESP_USE_STRLEN);
    }

    /* Hold the credentials in RAM. They reach NVS only if the join succeeds. */
    memset(&s_pending, 0, sizeof(s_pending));
    strncpy(s_pending.ssid,   ssid,        sizeof(s_pending.ssid) - 1);
    strncpy(s_pending.pass,   pass,        sizeof(s_pending.pass) - 1);
    strncpy(s_pending.server, server_addr, sizeof(s_pending.server) - 1);

    s_state = PORTAL_JOINING;
    s_fail_reason = 0;
    xTaskCreate(portal_join_task, "portal_join", 4096, NULL, 5, NULL);

    /* Answer immediately; the page polls /info for the outcome. */
    return httpd_resp_send(req, "Connecting...", HTTPD_RESP_USE_STRLEN);
}

/* Polled by the portal page for the outcome of the in-flight join. */
static esp_err_t handle_info(httpd_req_t *req)
{
    const char *state;
    switch (s_state) {
        case PORTAL_JOINING:   state = "joining";   break;
        case PORTAL_JOIN_OK:   state = "ok";        break;
        case PORTAL_JOIN_FAIL: state = "fail";      break;
        default:               state = "idle";      break;
    }

    char buf[96];
    int n = snprintf(buf, sizeof(buf), "{\"state\":\"%s\",\"reason\":%u}",
                     state, (unsigned)s_fail_reason);

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
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
    /* Also create STA netif so APSTA scan and the verify-join work */
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    /* Verify-join plumbing. The handler is muted unless a join is in flight,
     * so the AP's own client events never touch this state. */
    s_join_eg = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &portal_wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &portal_wifi_event_handler, NULL, NULL));

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, ap_ssid, sizeof(ap_config.ap.ssid));
    ap_config.ap.ssid_len = strlen(ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_OPEN;
    ap_config.ap.max_connection = 2;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    esp_wifi_set_max_tx_power(44);

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
    httpd_uri_t uri_info  = { .uri = "/info",  .method = HTTP_GET,  .handler = handle_info };
    httpd_uri_t uri_save  = { .uri = "/save",  .method = HTTP_POST, .handler = handle_save };
    httpd_uri_t uri_catch = { .uri = "/*",     .method = HTTP_GET,  .handler = handle_redirect };

    httpd_register_uri_handler(server, &uri_root);
    httpd_register_uri_handler(server, &uri_scan);
    httpd_register_uri_handler(server, &uri_info);
    httpd_register_uri_handler(server, &uri_save);
    httpd_register_uri_handler(server, &uri_catch);

    ESP_LOGI(TAG, "Captive portal running at http://192.168.4.1/");

    /* Block forever — reboot happens in handle_save */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
