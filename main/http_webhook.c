#include "http_webhook.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "webhook";

#define MAX_RETRIES     3
#define RETRY_DELAY_MS  2000
#define MAX_RESPONSE_LEN 512

static char s_server[128] = {0};

static void build_url(char *buf, size_t buf_len, const char *path)
{
    snprintf(buf, buf_len, "http://%s%s", s_server, path);
}

esp_err_t webhook_init(const char *server)
{
    if (!server || strlen(server) == 0) {
        ESP_LOGE(TAG, "Server address is empty");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_server, server, sizeof(s_server) - 1);
    ESP_LOGI(TAG, "Webhook initialized: server=%s", s_server);
    return ESP_OK;
}

esp_err_t webhook_set_server(const char *server)
{
    if (!server || strlen(server) == 0) return ESP_ERR_INVALID_ARG;
    strncpy(s_server, server, sizeof(s_server) - 1);
    ESP_LOGI(TAG, "Server updated: %s", s_server);
    return ESP_OK;
}

static void parse_response(const char *body, webhook_response_t *response)
{
    memset(response, 0, sizeof(*response));

    const char *user_ptr = strstr(body, "\"user\"");
    if (user_ptr) {
        const char *colon = strchr(user_ptr + 6, ':');
        if (colon) {
            const char *p = colon + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(response->user_name) - 1) {
                    response->user_name[i++] = *p++;
                }
                response->user_name[i] = '\0';
                if (i > 0) response->identified = true;
            }
        }
    }

    const char *conf_ptr = strstr(body, "\"confidence\"");
    if (conf_ptr) {
        const char *colon = strchr(conf_ptr + 12, ':');
        if (colon) response->confidence = strtof(colon + 1, NULL);
    }
}

typedef struct {
    char *buffer;
    int len;
    int max_len;
} http_response_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_response_ctx_t *ctx = (http_response_ctx_t *)evt->user_data;
    switch (evt->event_id) {
    case HTTP_EVENT_ON_DATA:
        if (ctx && ctx->buffer && evt->data_len > 0) {
            int copy_len = evt->data_len;
            if (ctx->len + copy_len >= ctx->max_len)
                copy_len = ctx->max_len - ctx->len - 1;
            if (copy_len > 0) {
                memcpy(ctx->buffer + ctx->len, evt->data, copy_len);
                ctx->len += copy_len;
                ctx->buffer[ctx->len] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return ESP_OK;
}

static esp_err_t post_json(const char *path, const char *payload,
                            char *resp_buf, int resp_buf_len)
{
    char url[256];
    build_url(url, sizeof(url), path);

    http_response_ctx_t resp_ctx = {
        .buffer = resp_buf,
        .len = 0,
        .max_len = resp_buf_len,
    };
    if (resp_buf) resp_buf[0] = '\0';

    esp_http_client_config_t config = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .event_handler = http_event_handler,
        .user_data = resp_buf ? &resp_ctx : NULL,
    };

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) return ESP_FAIL;

    esp_http_client_set_header(client, "Content-Type", "application/json");
    esp_http_client_set_post_field(client, payload, strlen(payload));

    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err == ESP_OK && (status >= 200 && status < 300)) return ESP_OK;

    ESP_LOGW(TAG, "POST %s failed: err=%s status=%d", path, esp_err_to_name(err), status);
    return ESP_FAIL;
}

esp_err_t webhook_post_measurement(const scale_measurement_t *m, webhook_response_t *response)
{
    if (strlen(s_server) == 0) {
        ESP_LOGE(TAG, "No server configured");
        return ESP_FAIL;
    }

    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"weight_kg\":%.2f,\"weight_lb\":%.2f,\"impedance\":%d,\"battery\":%d}",
             m->weight_kg, m->weight_kg * 2.20462f,
             m->impedance_ohms, m->battery_pct);

    char resp_buf[MAX_RESPONSE_LEN] = {0};

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retry %d/%d...", attempt + 1, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }

        if (post_json("/api/webhook/measurement", payload, resp_buf, MAX_RESPONSE_LEN) == ESP_OK) {
            ESP_LOGI(TAG, "Measurement POST success");
            if (response && strlen(resp_buf) > 0) {
                parse_response(resp_buf, response);
            }
            return ESP_OK;
        }
    }

    ESP_LOGE(TAG, "Measurement POST failed after %d retries", MAX_RETRIES);
    return ESP_FAIL;
}

esp_err_t webhook_post_log(const char *level, const char *tag, const char *message)
{
    if (strlen(s_server) == 0) return ESP_FAIL;

    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"level\":\"%s\",\"tag\":\"%s\",\"message\":\"%s\"}",
             level, tag, message);

    return post_json("/api/webhook/log", payload, NULL, 0);
}
