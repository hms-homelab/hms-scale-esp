#include "scale_ble_client.h"
#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_gap_ble_api.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include "esp_bt_main.h"
#include "esp_gatt_common_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "nvs.h"

static const char *TAG = "scale_ble";

// Etekcity Scale UUIDs (16-bit format based on actual device discovery)
// Service: 0xFFF0 (not 0xFFF1 as in some docs!)
// Characteristic: 0xFFF1 (weight/impedance notifications)
#define SCALE_SERVICE_UUID        0xFFF0
#define SCALE_CHARACTERISTIC_UUID 0xFFF1
#define SCALE_CCCD_UUID           0x2902  // Client Characteristic Configuration Descriptor

// BLE connection parameters
static esp_gatt_if_t g_gattc_if = ESP_GATT_IF_NONE;
static uint16_t g_conn_id = 0;
static esp_bd_addr_t g_scale_mac;
static uint16_t g_notify_handle = 0;
static uint16_t g_cccd_handle = 0;
static uint16_t g_service_start_handle = 0;
static uint16_t g_service_end_handle = 0;

// Connection state
static bool g_connected = false;
static bool g_scanning = false;

// User callback
static scale_measurement_callback_t g_measurement_cb = NULL;

// User profile for body metrics (initialized from NVS or Kconfig defaults)
static int g_user_age = CONFIG_USER_AGE;
static float g_user_height_cm = CONFIG_USER_HEIGHT_CM;
static bool g_user_is_male = CONFIG_USER_SEX_MALE;

#define NVS_NAMESPACE "scale_user"
#define NVS_KEY_AGE "age"
#define NVS_KEY_HEIGHT "height"
#define NVS_KEY_SEX "sex_male"

// Forward declarations
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param);
static void parse_scale_data(const uint8_t *data, size_t len);
static void calculate_body_metrics(const scale_measurement_t *measurement, body_metrics_t *metrics);
static esp_err_t load_user_profile_from_nvs(void);
static esp_err_t save_user_profile_to_nvs(void);

// Convert MAC address string to bytes
static bool parse_mac_address(const char *mac_str, uint8_t *mac_bytes) {
    int values[6];
    if (sscanf(mac_str, "%x:%x:%x:%x:%x:%x",
               &values[0], &values[1], &values[2],
               &values[3], &values[4], &values[5]) == 6) {
        for (int i = 0; i < 6; i++) {
            mac_bytes[i] = (uint8_t)values[i];
        }
        return true;
    }
    return false;
}

// Compare MAC addresses
static bool mac_addresses_match(const uint8_t *mac1, const uint8_t *mac2) {
    return memcmp(mac1, mac2, 6) == 0;
}

// GAP event handler
static void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan parameters set, starting scan...");
        esp_ble_gap_start_scanning(0);  // 0 = scan indefinitely
        break;

    case ESP_GAP_BLE_SCAN_START_COMPLETE_EVT:
        if (param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "🦒 Scanning for Etekcity scale...");
            g_scanning = true;
        } else {
            ESP_LOGE(TAG, "Scan start failed: %d", param->scan_start_cmpl.status);
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt == ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Check if this is our scale
            if (mac_addresses_match(param->scan_rst.bda, g_scale_mac)) {
                ESP_LOGI(TAG, "🔍 Found Etekcity scale! RSSI: %d dB", param->scan_rst.rssi);

                // Stop scanning
                esp_ble_gap_stop_scanning();
                g_scanning = false;

                // Connect to the scale
                ESP_LOGI(TAG, "Connecting to scale...");
                esp_ble_gattc_open(g_gattc_if, param->scan_rst.bda, param->scan_rst.ble_addr_type, true);
            }
        }
        break;

    case ESP_GAP_BLE_SCAN_STOP_COMPLETE_EVT:
        ESP_LOGI(TAG, "Scan stopped");
        g_scanning = false;
        break;

    default:
        break;
    }
}

// GATTC event handler
static void gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    switch (event) {
    case ESP_GATTC_REG_EVT:
        ESP_LOGI(TAG, "GATT client registered, app_id: %d", param->reg.app_id);
        g_gattc_if = gattc_if;

        // Set scan parameters
        esp_ble_gap_set_scan_params(&(esp_ble_scan_params_t){
            .scan_type = BLE_SCAN_TYPE_ACTIVE,
            .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
            .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
            .scan_interval = 0x50,  // 50ms
            .scan_window = 0x30     // 30ms
        });
        break;

    case ESP_GATTC_OPEN_EVT:
        if (param->open.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "✅ Connected to scale!");
            g_conn_id = param->open.conn_id;
            g_connected = true;
            memcpy(g_scale_mac, param->open.remote_bda, 6);

            // Discover services
            ESP_LOGI(TAG, "Discovering services...");
            esp_ble_gattc_search_service(gattc_if, param->open.conn_id, NULL);
        } else {
            ESP_LOGE(TAG, "Connection failed: %d", param->open.status);
            // Resume scanning
            esp_ble_gap_start_scanning(0);
        }
        break;

    case ESP_GATTC_SEARCH_RES_EVT: {
        // Found a service
        esp_gatt_id_t *srvc_id = &param->search_res.srvc_id;

        // Log all discovered services for debugging
        if (srvc_id->uuid.len == ESP_UUID_LEN_16) {
            ESP_LOGI(TAG, "Found service: 16-bit UUID 0x%04x (handles 0x%04x-0x%04x)",
                     srvc_id->uuid.uuid.uuid16,
                     param->search_res.start_handle,
                     param->search_res.end_handle);
        } else if (srvc_id->uuid.len == ESP_UUID_LEN_128) {
            ESP_LOGI(TAG, "Found service: 128-bit UUID (handles 0x%04x-0x%04x)",
                     param->search_res.start_handle,
                     param->search_res.end_handle);
            ESP_LOG_BUFFER_HEX(TAG, srvc_id->uuid.uuid.uuid128, 16);
        }

        // Check if this is our scale service (16-bit UUID 0xFFF0)
        if (srvc_id->uuid.len == ESP_UUID_LEN_16 &&
            srvc_id->uuid.uuid.uuid16 == SCALE_SERVICE_UUID) {
            ESP_LOGI(TAG, "✅ Found scale service 0xFFF0!");
            g_service_start_handle = param->search_res.start_handle;
            g_service_end_handle = param->search_res.end_handle;
            ESP_LOGI(TAG, "Service handles: start=0x%04x, end=0x%04x",
                     g_service_start_handle, g_service_end_handle);
        }
        break;
    }

    case ESP_GATTC_SEARCH_CMPL_EVT:
        ESP_LOGI(TAG, "Service discovery complete");

        if (g_service_start_handle != 0) {
            ESP_LOGI(TAG, "Getting characteristics for scale service (0x%04x - 0x%04x)...",
                     g_service_start_handle, g_service_end_handle);

            // Get count of characteristics
            uint16_t count = 0;
            esp_gatt_status_t status = esp_ble_gattc_get_attr_count(
                gattc_if,
                g_conn_id,
                ESP_GATT_DB_CHARACTERISTIC,
                g_service_start_handle,
                g_service_end_handle,
                0,  // char_handle (0 = all)
                &count);

            ESP_LOGI(TAG, "Attr count result: status=%d, count=%d", status, count);

            if (status == ESP_GATT_OK && count > 0) {
                ESP_LOGI(TAG, "Found %d characteristics", count);

                // Allocate memory for characteristics
                esp_gattc_char_elem_t *char_elem_result = malloc(sizeof(esp_gattc_char_elem_t) * count);
                if (char_elem_result == NULL) {
                    ESP_LOGE(TAG, "Failed to allocate memory for characteristics");
                    break;
                }

                // Get all characteristics
                status = esp_ble_gattc_get_all_char(
                    gattc_if,
                    g_conn_id,
                    g_service_start_handle,
                    g_service_end_handle,
                    char_elem_result,
                    &count,
                    0);

                if (status == ESP_GATT_OK) {
                    for (int i = 0; i < count; i++) {
                        uint16_t char_uuid = (char_elem_result[i].uuid.len == ESP_UUID_LEN_16) ?
                                              char_elem_result[i].uuid.uuid.uuid16 : 0;
                        uint16_t char_handle = char_elem_result[i].char_handle;
                        uint8_t char_prop = char_elem_result[i].properties;

                        ESP_LOGI(TAG, "Char[%d]: UUID 0x%04x, handle 0x%04x, prop 0x%02x",
                                 i, char_uuid, char_handle, char_prop);

                        // Check if this is the scale characteristic (16-bit UUID 0xFFF1)
                        if (char_uuid == SCALE_CHARACTERISTIC_UUID) {
                            g_notify_handle = char_handle;
                            ESP_LOGI(TAG, "✅ Found scale characteristic at handle 0x%04x", g_notify_handle);

                            // Register for notifications
                            esp_err_t ret = esp_ble_gattc_register_for_notify(gattc_if, g_scale_mac, char_handle);
                            if (ret == ESP_OK) {
                                ESP_LOGI(TAG, "Registered for notifications");
                            } else {
                                ESP_LOGE(TAG, "Failed to register for notifications: %s", esp_err_to_name(ret));
                            }

                            // Enable notifications via CCCD (usually at char_handle + 1)
                            g_cccd_handle = char_handle + 1;
                            uint16_t notify_en = 1;  // 0x0001 = notifications enabled
                            ESP_LOGI(TAG, "Writing to CCCD at handle 0x%04x to enable notifications", g_cccd_handle);

                            ret = esp_ble_gattc_write_char_descr(
                                gattc_if,
                                g_conn_id,
                                g_cccd_handle,
                                sizeof(notify_en),
                                (uint8_t*)&notify_en,
                                ESP_GATT_WRITE_TYPE_RSP,
                                ESP_GATT_AUTH_REQ_NONE);

                            if (ret == ESP_OK) {
                                ESP_LOGI(TAG, "📡 Enabling notifications...");
                            } else {
                                ESP_LOGE(TAG, "Failed to write CCCD: %s", esp_err_to_name(ret));
                            }
                        }
                    }
                } else {
                    ESP_LOGE(TAG, "Failed to get characteristics: %d", status);
                }

                free(char_elem_result);
            } else {
                ESP_LOGW(TAG, "No characteristics found or error: status=%d, count=%d", status, count);
            }
        } else {
            ESP_LOGW(TAG, "Scale service 0xFFF0 not found!");
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status == ESP_GATT_OK) {
            ESP_LOGI(TAG, "✅ Notifications enabled! Waiting for scale measurements...");
        } else {
            ESP_LOGE(TAG, "Failed to enable notifications: %d", param->write.status);
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        // Received notification from scale!
        ESP_LOGI(TAG, "📊 Received %d bytes from scale", param->notify.value_len);
        parse_scale_data(param->notify.value, param->notify.value_len);
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGI(TAG, "⏸️  Disconnected from scale");
        g_connected = false;
        g_conn_id = 0;
        g_notify_handle = 0;
        g_cccd_handle = 0;

        // Resume scanning after 2 seconds
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_ble_gap_start_scanning(0);
        break;

    default:
        break;
    }
}

// Parse the 22-byte scale data packet
static void parse_scale_data(const uint8_t *data, size_t len) {
    if (len != 22) {
        ESP_LOGW(TAG, "Invalid packet size: %zu (expected 22)", len);
        return;
    }

    // Validate header
    if (data[0] != 0xA5 || data[1] != 0x02) {
        ESP_LOGW(TAG, "Invalid header: 0x%02X 0x%02X", data[0], data[1]);
        return;
    }

    // Parse measurement data
    scale_measurement_t measurement = {0};

    // Check stability flag (byte 19)
    measurement.stable = (data[19] == 0x01);

    // Weight (bytes 10-12, little-endian, in grams)
    uint32_t weight_raw = data[10] | (data[11] << 8) | (data[12] << 16);
    measurement.weight_kg = weight_raw / 1000.0f;

    // Impedance (bytes 13-14, little-endian)
    measurement.impedance_ohms = data[13] | (data[14] << 8);
    measurement.impedance_valid = (data[20] == 0x01);

    // Battery (byte 18)
    measurement.battery_pct = data[18];
    if (measurement.battery_pct > 100) measurement.battery_pct = 100;

    // Lock-on-stability logic: Only publish stable measurements with meaningful weight
    // This prevents publishing 0 values when stepping off the scale
    static float last_locked_weight = 0.0f;

    // Minimum weight threshold (5kg) to filter out noise and empty scale readings
    const float MIN_WEIGHT_KG = 5.0f;

    // Only publish when:
    // 1. Measurement is stable (byte 19 == 0x01)
    // 2. Weight is above minimum threshold (someone is actually on the scale)
    // 3. Weight is different from last locked value (new person or significant change)
    bool is_meaningful = measurement.weight_kg >= MIN_WEIGHT_KG;
    bool is_new_measurement = fabs(measurement.weight_kg - last_locked_weight) > 0.5f;

    if (!measurement.stable || !is_meaningful || !is_new_measurement) {
        // Log unstable or empty measurements but don't publish
        ESP_LOGD(TAG, "Skipping: Weight %.2f kg, Stable: %s, Meaningful: %s",
                 measurement.weight_kg,
                 measurement.stable ? "YES" : "NO",
                 is_meaningful ? "YES" : "NO");
        return;
    }

    // Lock in this stable measurement
    last_locked_weight = measurement.weight_kg;

    ESP_LOGI(TAG, "⚖️  Weight: %.2f kg, Impedance: %d Ω, Battery: %d%%, Stable: %s",
             measurement.weight_kg, measurement.impedance_ohms, measurement.battery_pct,
             measurement.stable ? "YES" : "NO");

    // Calculate body metrics
    body_metrics_t metrics = {0};
    calculate_body_metrics(&measurement, &metrics);

    ESP_LOGI(TAG, "📊 BMI: %.1f, Body Fat: %.1f%%, BMR: %.0f kcal/day",
             metrics.bmi, metrics.body_fat_pct, metrics.bmr_kcal);

    // Call user callback
    if (g_measurement_cb) {
        g_measurement_cb(&measurement, &metrics);
    }
}

// Calculate body metrics using measurement data
static void calculate_body_metrics(const scale_measurement_t *measurement, body_metrics_t *metrics) {
    float height_m = g_user_height_cm / 100.0f;

    // BMI = weight / height²
    metrics->bmi = measurement->weight_kg / (height_m * height_m);

    // Body Fat % (Deurenberg formula)
    // BF% = 1.20 × BMI + 0.23 × Age - 10.8 × Sex - 5.4
    metrics->body_fat_pct = 1.20f * metrics->bmi + 0.23f * g_user_age
                           - 10.8f * (g_user_is_male ? 1.0f : 0.0f) - 5.4f;

    // Clamp to realistic range
    if (metrics->body_fat_pct < 0.0f) metrics->body_fat_pct = 0.0f;
    if (metrics->body_fat_pct > 50.0f) metrics->body_fat_pct = 50.0f;

    // Fat Mass = weight × (body_fat% / 100)
    metrics->fat_mass_kg = measurement->weight_kg * (metrics->body_fat_pct / 100.0f);

    // Lean Mass = weight - fat_mass
    metrics->lean_mass_kg = measurement->weight_kg - metrics->fat_mass_kg;

    // BMR (Mifflin-St Jeor equation)
    // Male: BMR = 10×weight + 6.25×height - 5×age + 5
    // Female: BMR = 10×weight + 6.25×height - 5×age - 161
    metrics->bmr_kcal = 10.0f * measurement->weight_kg
                       + 6.25f * g_user_height_cm
                       - 5.0f * g_user_age;
    metrics->bmr_kcal += g_user_is_male ? 5.0f : -161.0f;

    // Extended Body Composition Metrics (BIA formulas)

    // Body Water % (based on lean mass and age/gender)
    // Typical: Male 60-65%, Female 50-60% of total body weight
    // Formula: (0.621 × lean_mass + age_factor + gender_factor)
    float age_factor = -0.1f * (g_user_age / 10.0f);  // Decreases with age
    float gender_factor = g_user_is_male ? 2.5f : -2.5f;
    metrics->body_water_pct = (0.621f * metrics->lean_mass_kg / measurement->weight_kg * 100.0f)
                             + age_factor + gender_factor;
    if (metrics->body_water_pct < 35.0f) metrics->body_water_pct = 35.0f;
    if (metrics->body_water_pct > 75.0f) metrics->body_water_pct = 75.0f;

    // Protein % (typically 15-20% of body weight)
    // Formula: (lean_mass × 0.18) / weight
    metrics->protein_pct = (metrics->lean_mass_kg * 0.18f / measurement->weight_kg) * 100.0f;
    if (metrics->protein_pct < 10.0f) metrics->protein_pct = 10.0f;
    if (metrics->protein_pct > 25.0f) metrics->protein_pct = 25.0f;

    // Bone Mass (kg) - based on weight and gender
    // Formula: weight × bone_coefficient + adjustment
    float bone_coeff = g_user_is_male ? 0.018f : 0.015f;
    metrics->bone_mass_kg = measurement->weight_kg * bone_coeff;
    if (metrics->bone_mass_kg < 1.5f) metrics->bone_mass_kg = 1.5f;
    if (metrics->bone_mass_kg > 5.0f) metrics->bone_mass_kg = 5.0f;

    // Muscle Mass (kg) = Lean Mass - Bone Mass
    metrics->muscle_mass_kg = metrics->lean_mass_kg - metrics->bone_mass_kg;
    if (metrics->muscle_mass_kg < 0.0f) metrics->muscle_mass_kg = 0.0f;

    // Skeletal Muscle % (muscle that can be increased through exercise)
    // Typical: Male 38-44%, Female 28-35%
    // Formula: (muscle_mass / weight) × adjustment_factor
    float skeletal_factor = g_user_is_male ? 0.85f : 0.75f;
    metrics->skeletal_muscle_pct = (metrics->muscle_mass_kg / measurement->weight_kg * skeletal_factor) * 100.0f;
    if (metrics->skeletal_muscle_pct < 20.0f) metrics->skeletal_muscle_pct = 20.0f;
    if (metrics->skeletal_muscle_pct > 50.0f) metrics->skeletal_muscle_pct = 50.0f;

    // Subcutaneous Fat % (fat under the skin, typically 50-85% of total body fat)
    // Formula: body_fat × 0.75 (average 75% of fat is subcutaneous)
    metrics->subcutaneous_fat_pct = metrics->body_fat_pct * 0.75f;

    // Visceral Fat (rating 1-59, based on body fat and BMI)
    // Formula: (bmi_factor + fat_factor + age_factor) / 2
    float bmi_factor = (metrics->bmi - 20.0f) * 0.5f;  // Increases with BMI above 20
    float fat_factor = (metrics->body_fat_pct - 15.0f) * 0.3f;  // Increases with body fat
    float visc_age_factor = g_user_age * 0.05f;  // Increases with age
    metrics->visceral_fat = bmi_factor + fat_factor + visc_age_factor;
    if (metrics->visceral_fat < 1.0f) metrics->visceral_fat = 1.0f;
    if (metrics->visceral_fat > 59.0f) metrics->visceral_fat = 59.0f;

    // Metabolic Age (years) - based on BMR comparison
    // Formula: Compare actual BMR to expected BMR for age 18-80
    // Higher BMR = younger metabolic age
    float expected_bmr_at_25 = 10.0f * measurement->weight_kg + 6.25f * g_user_height_cm - 5.0f * 25.0f;
    expected_bmr_at_25 += g_user_is_male ? 5.0f : -161.0f;

    float bmr_ratio = metrics->bmr_kcal / expected_bmr_at_25;
    metrics->metabolic_age = (uint8_t)(25.0f + (1.0f - bmr_ratio) * 30.0f);  // Age shifts from BMR

    // Clamp metabolic age to reasonable range
    if (metrics->metabolic_age < 18) metrics->metabolic_age = 18;
    if (metrics->metabolic_age > 80) metrics->metabolic_age = 80;
}

// NVS Storage Functions

static esp_err_t load_user_profile_from_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No saved user profile, using defaults: age=%d, height=%.0fcm, sex=%s",
                 g_user_age, g_user_height_cm, g_user_is_male ? "male" : "female");
        return ESP_OK;  // First time, use Kconfig defaults
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS: %s", esp_err_to_name(err));
        return err;
    }

    // Load age (default to Kconfig value if not found)
    int32_t age;
    err = nvs_get_i32(nvs_handle, NVS_KEY_AGE, &age);
    if (err == ESP_OK) {
        g_user_age = age;
    }

    // Load height (stored as integer cm)
    int32_t height;
    err = nvs_get_i32(nvs_handle, NVS_KEY_HEIGHT, &height);
    if (err == ESP_OK) {
        g_user_height_cm = (float)height;
    }

    // Load sex
    uint8_t sex_male;
    err = nvs_get_u8(nvs_handle, NVS_KEY_SEX, &sex_male);
    if (err == ESP_OK) {
        g_user_is_male = (bool)sex_male;
    }

    nvs_close(nvs_handle);

    ESP_LOGI(TAG, "✅ Loaded user profile from NVS: age=%d, height=%.0fcm, sex=%s",
             g_user_age, g_user_height_cm, g_user_is_male ? "male" : "female");

    return ESP_OK;
}

static esp_err_t save_user_profile_to_nvs(void) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Error opening NVS for write: %s", esp_err_to_name(err));
        return err;
    }

    // Save age
    err = nvs_set_i32(nvs_handle, NVS_KEY_AGE, g_user_age);
    if (err != ESP_OK) goto cleanup;

    // Save height (as integer cm)
    err = nvs_set_i32(nvs_handle, NVS_KEY_HEIGHT, (int32_t)g_user_height_cm);
    if (err != ESP_OK) goto cleanup;

    // Save sex
    err = nvs_set_u8(nvs_handle, NVS_KEY_SEX, g_user_is_male ? 1 : 0);
    if (err != ESP_OK) goto cleanup;

    // Commit changes
    err = nvs_commit(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "💾 User profile saved to NVS");
    }

cleanup:
    nvs_close(nvs_handle);
    return err;
}

// Public API functions

esp_err_t scale_ble_init(const char *mac_address, scale_measurement_callback_t callback) {
    ESP_LOGI(TAG, "Initializing BLE client for scale: %s", mac_address);

    // Parse MAC address
    if (!parse_mac_address(mac_address, g_scale_mac)) {
        ESP_LOGE(TAG, "Invalid MAC address format");
        return ESP_ERR_INVALID_ARG;
    }

    g_measurement_cb = callback;

    // Load user profile from NVS (or use Kconfig defaults)
    load_user_profile_from_nvs();

    // Register GATT client
    esp_err_t ret = esp_ble_gattc_register_callback(gattc_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATT client register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gap_register_callback(gap_event_handler);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GAP register callback failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ret = esp_ble_gattc_app_register(0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "GATT client app register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "BLE client initialized");
    return ESP_OK;
}

esp_err_t scale_ble_start_scan(void) {
    if (g_gattc_if == ESP_GATT_IF_NONE) {
        ESP_LOGW(TAG, "BLE not initialized yet");
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Starting BLE scan...");
    return esp_ble_gap_start_scanning(0);
}

esp_err_t scale_ble_stop(void) {
    if (g_scanning) {
        esp_ble_gap_stop_scanning();
    }
    if (g_connected) {
        esp_ble_gattc_close(g_gattc_if, g_conn_id);
    }
    return ESP_OK;
}

bool scale_ble_is_connected(void) {
    return g_connected;
}

// User Profile Configuration API

esp_err_t scale_ble_set_user_age(int age) {
    if (age < 10 || age > 120) {
        ESP_LOGE(TAG, "Invalid age: %d (must be 10-120)", age);
        return ESP_ERR_INVALID_ARG;
    }
    g_user_age = age;
    ESP_LOGI(TAG, "Updated user age: %d", age);
    return save_user_profile_to_nvs();
}

esp_err_t scale_ble_set_user_height(int height_cm) {
    if (height_cm < 100 || height_cm > 250) {
        ESP_LOGE(TAG, "Invalid height: %d (must be 100-250cm)", height_cm);
        return ESP_ERR_INVALID_ARG;
    }
    g_user_height_cm = (float)height_cm;
    ESP_LOGI(TAG, "Updated user height: %dcm", height_cm);
    return save_user_profile_to_nvs();
}

esp_err_t scale_ble_set_user_sex(bool is_male) {
    g_user_is_male = is_male;
    ESP_LOGI(TAG, "Updated user sex: %s", is_male ? "male" : "female");
    return save_user_profile_to_nvs();
}

void scale_ble_get_user_profile(int *age, int *height_cm, bool *is_male) {
    if (age) *age = g_user_age;
    if (height_cm) *height_cm = (int)g_user_height_cm;
    if (is_male) *is_male = g_user_is_male;
}
