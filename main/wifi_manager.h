#pragma once

#include "esp_err.h"
#include "esp_wifi_types.h"
#include <stdbool.h>

esp_err_t wifi_manager_init(void);
esp_err_t wifi_manager_connect(const char *ssid, const char *password);
esp_err_t wifi_manager_wait_connected(uint32_t timeout_ms);
bool wifi_manager_is_connected(void);
bool wifi_manager_is_auth_failure(void);
esp_err_t wifi_manager_scan(wifi_ap_record_t *results, uint16_t *count, uint16_t max_count);
