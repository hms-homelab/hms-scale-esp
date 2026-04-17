#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

bool nvs_config_has_wifi(void);
bool nvs_config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t nvs_config_set_wifi(const char *ssid, const char *pass);
void nvs_config_clear(void);

bool nvs_config_get_webhook_url(char *url, size_t url_len);
esp_err_t nvs_config_set_webhook_url(const char *url);
