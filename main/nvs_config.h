#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

bool nvs_config_has_wifi(void);
bool nvs_config_get_wifi(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
esp_err_t nvs_config_set_wifi(const char *ssid, const char *pass);
void nvs_config_clear(void);

// Server is stored as "host:port" (e.g. "192.168.2.15:8889")
// ESP32 builds full URLs: http://{server}/api/webhook/measurement, /api/webhook/log
bool nvs_config_get_server(char *server, size_t server_len);
esp_err_t nvs_config_set_server(const char *server);
