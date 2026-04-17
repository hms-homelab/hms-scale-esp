#pragma once

#include "scale_ble_client.h"
#include "esp_err.h"

typedef struct {
    char user_name[64];
    float confidence;
    bool identified;
} webhook_response_t;

// server = "192.168.2.15:8889" — URLs built as http://{server}/api/webhook/...
esp_err_t webhook_init(const char *server);
esp_err_t webhook_set_server(const char *server);
esp_err_t webhook_post_measurement(const scale_measurement_t *m, webhook_response_t *response);
esp_err_t webhook_post_log(const char *level, const char *tag, const char *message);
