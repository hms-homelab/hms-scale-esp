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

static char s_webhook_url[256] = {0};

esp_err_t webhook_init(const char *url)
{
    if (!url || strlen(url) == 0) {
        ESP_LOGE(TAG, "Webhook URL is empty");
        return ESP_ERR_INVALID_ARG;
    }
    strncpy(s_webhook_url, url, sizeof(s_webhook_url) - 1);
    ESP_LOGI(TAG, "Webhook initialized: %s", s_webhook_url);
    return ESP_OK;
}

esp_err_t webhook_set_url(const char *url)
{
    if (!url || strlen(url) == 0) return ESP_ERR_INVALID_ARG;
    strncpy(s_webhook_url, url, sizeof(s_webhook_url) - 1);
    ESP_LOGI(TAG, "Webhook URL updated: %s", s_webhook_url);
    return ESP_OK;
}

/* Parse simple JSON response: {"status":"ok","user":"NAME","confidence":95.5}
 * Uses strstr/sscanf — no JSON library needed. */
static void parse_response(const char *body, webhook_response_t *response)
{
    memset(response, 0, sizeof(*response));

    /* Check for "user" field */
    const char *user_ptr = strstr(body, "\"user\"");
    if (user_ptr) {
        /* Find the value after the colon and opening quote */
        const char *colon = strchr(user_ptr + 6, ':');
        if (colon) {
            /* Skip whitespace and opening quote */
            const char *p = colon + 1;
            while (*p == ' ' || *p == '\t') p++;
            if (*p == '"') {
                p++;
                /* Copy until closing quote */
                size_t i = 0;
                while (*p && *p != '"' && i < sizeof(response->user_name) - 1) {
                    response->user_name[i++] = *p++;
                }
                response->user_name[i] = '\0';
                if (i > 0) {
                    response->identified = true;
                }
            }
        }
    }

    /* Check for "confidence" field */
    const char *conf_ptr = strstr(body, "\"confidence\"");
    if (conf_ptr) {
        const char *colon = strchr(conf_ptr + 12, ':');
        if (colon) {
            response->confidence = strtof(colon + 1, NULL);
        }
    }
}

/* HTTP event handler to capture response body */
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
            if (ctx->len + copy_len >= ctx->max_len) {
                copy_len = ctx->max_len - ctx->len - 1;
            }
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

esp_err_t webhook_post_measurement(const scale_measurement_t *m, webhook_response_t *response)
{
    if (strlen(s_webhook_url) == 0) {
        ESP_LOGE(TAG, "No webhook URL configured");
        return ESP_FAIL;
    }

    /* Build JSON payload */
    char payload[256];
    snprintf(payload, sizeof(payload),
             "{\"weight_kg\":%.2f,\"weight_lb\":%.2f,\"impedance\":%d,\"battery\":%d}",
             m->weight_kg,
             m->weight_kg * 2.20462f,
             m->impedance_ohms,
             m->battery_pct);

    /* Response buffer */
    char resp_buf[MAX_RESPONSE_LEN] = {0};
    http_response_ctx_t resp_ctx = {
        .buffer = resp_buf,
        .len = 0,
        .max_len = MAX_RESPONSE_LEN,
    };

    for (int attempt = 0; attempt < MAX_RETRIES; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "Retry %d/%d...", attempt + 1, MAX_RETRIES);
            vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
        }

        /* Reset response buffer */
        resp_ctx.len = 0;
        resp_buf[0] = '\0';

        esp_http_client_config_t config = {
            .url = s_webhook_url,
            .method = HTTP_METHOD_POST,
            .timeout_ms = 10000,
            .event_handler = http_event_handler,
            .user_data = &resp_ctx,
        };

        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            ESP_LOGE(TAG, "Failed to init HTTP client");
            continue;
        }

        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, payload, strlen(payload));

        esp_err_t err = esp_http_client_perform(client);
        int status_code = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (err == ESP_OK && status_code == 200) {
            ESP_LOGI(TAG, "Webhook POST success (HTTP 200)");
            if (response && resp_ctx.len > 0) {
                parse_response(resp_buf, response);
            }
            return ESP_OK;
        }

        ESP_LOGW(TAG, "Webhook POST failed: err=%s, status=%d",
                 esp_err_to_name(err), status_code);
    }

    ESP_LOGE(TAG, "Webhook POST failed after %d retries", MAX_RETRIES);
    return ESP_FAIL;
}
