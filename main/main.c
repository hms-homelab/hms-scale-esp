#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_bt.h"
#include "esp_bt_main.h"

#include "scale_ble_client.h"
#include "nvs_config.h"
#include "wifi_manager.h"
#include "captive_portal.h"
#include "http_webhook.h"
#include "web_config.h"

static const char *TAG = "main";

/* Forward declaration for web_config last-measurement update */
extern void web_config_set_last_measurement(float weight_kg);

/* Scale measurement callback */
static void scale_measurement_handler(const scale_measurement_t *measurement, const body_metrics_t *metrics)
{
    ESP_LOGI(TAG, "Giraffe Scale Measurement:");
    ESP_LOGI(TAG, "  Weight: %.2f kg (%.2f lb)",
             measurement->weight_kg, measurement->weight_kg * 2.20462);
    ESP_LOGI(TAG, "  Impedance: %d ohm", measurement->impedance_ohms);
    ESP_LOGI(TAG, "  Battery: %d%%", measurement->battery_pct);
    ESP_LOGI(TAG, "  BMI: %.1f | Body Fat: %.1f%% | Muscle: %.1f kg",
             metrics->bmi, metrics->body_fat_pct, metrics->muscle_mass_kg);
    ESP_LOGI(TAG, "  Body Water: %.1f%% | Bone: %.2f kg | Protein: %.1f%%",
             metrics->body_water_pct, metrics->bone_mass_kg, metrics->protein_pct);
    ESP_LOGI(TAG, "  Visceral Fat: %.1f | Metabolic Age: %d years | BMR: %.0f kcal/day",
             metrics->visceral_fat, metrics->metabolic_age, metrics->bmr_kcal);

    /* Update web config status page */
    web_config_set_last_measurement(measurement->weight_kg);

    /* Send to hms-scale via webhook (replaces MQTT publish) */
    webhook_response_t response = {0};
    esp_err_t ret = webhook_post_measurement(measurement, &response);
    if (ret == ESP_OK) {
        if (response.identified) {
            ESP_LOGI(TAG, "  -> Identified as %s (%.1f%% confidence)",
                     response.user_name, response.confidence);
        } else {
            ESP_LOGI(TAG, "  -> User not identified (unassigned)");
        }
    } else {
        ESP_LOGE(TAG, "  -> Webhook POST failed after retries");
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "Giraffe Scale v2.0.0 starting...");

    /* 1. Init NVS */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Check for WiFi config */
    if (!nvs_config_has_wifi()) {
        ESP_LOGI(TAG, "No WiFi configured -- starting captive portal");
        captive_portal_start(); /* blocks, reboots after save */
        return; /* never reached */
    }

    /* 3. Load config from NVS */
    char ssid[33] = {0}, pass[65] = {0}, webhook_url[256] = {0};
    nvs_config_get_wifi(ssid, sizeof(ssid), pass, sizeof(pass));
    if (!nvs_config_get_webhook_url(webhook_url, sizeof(webhook_url))) {
        strncpy(webhook_url, CONFIG_DEFAULT_WEBHOOK_URL, sizeof(webhook_url) - 1);
    }

    /* 4. Connect WiFi */
    wifi_manager_init();
    wifi_manager_connect(ssid, pass);
    if (wifi_manager_wait_connected(30000) != ESP_OK) {
        ESP_LOGE(TAG, "WiFi connection failed -- starting captive portal");
        nvs_config_clear();
        esp_restart();
        return;
    }

    /* 5. Init webhook */
    webhook_init(webhook_url);

    /* 6. Start station-mode config server */
    web_config_start();

    /* 7. Init Bluetooth */
    ESP_LOGI(TAG, "Initializing Bluetooth...");
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_CLASSIC_BT));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    ret = esp_bt_controller_init(&bt_cfg);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bt_controller_enable(ESP_BT_MODE_BLE);
    if (ret) {
        ESP_LOGE(TAG, "Bluetooth controller enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_init();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid init failed: %s", esp_err_to_name(ret));
        return;
    }

    ret = esp_bluedroid_enable();
    if (ret) {
        ESP_LOGE(TAG, "Bluedroid enable failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Bluetooth initialized");

    /* 8. Init BLE scale client */
    ret = scale_ble_init(CONFIG_SCALE_MAC_ADDRESS, scale_measurement_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Scale BLE init failed: %s", esp_err_to_name(ret));
        return;
    }

    ESP_LOGI(TAG, "Giraffe Scale ready -- webhook: %s", webhook_url);
    ESP_LOGI(TAG, "Scanning for scale at MAC: %s", CONFIG_SCALE_MAC_ADDRESS);

    /* Main loop */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));

        if (scale_ble_is_connected()) {
            ESP_LOGI(TAG, "Scale connected - waiting for measurements");
        } else {
            ESP_LOGI(TAG, "Scanning for scale...");
        }
    }
}
