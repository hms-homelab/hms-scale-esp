#pragma once

#include "scale_ble_client.h"
#include "esp_err.h"

typedef struct {
    char user_name[64];
    float confidence;
    bool identified;
} webhook_response_t;

esp_err_t webhook_init(const char *url);
esp_err_t webhook_set_url(const char *url);
esp_err_t webhook_post_measurement(const scale_measurement_t *m, webhook_response_t *response);
