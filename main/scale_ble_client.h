#ifndef SCALE_BLE_CLIENT_H
#define SCALE_BLE_CLIENT_H

#include "esp_err.h"
#include <stdint.h>
#include <stdbool.h>

// Scale measurement data structure
typedef struct {
    float weight_kg;
    uint16_t impedance_ohms;
    uint8_t battery_pct;
    bool stable;
    bool impedance_valid;
} scale_measurement_t;

// Body metrics structure
typedef struct {
    float bmi;
    float body_fat_pct;
    float fat_mass_kg;
    float lean_mass_kg;
    float bmr_kcal;

    // Extended body composition metrics
    float muscle_mass_kg;
    float skeletal_muscle_pct;
    float body_water_pct;
    float bone_mass_kg;
    float protein_pct;
    float visceral_fat;
    float subcutaneous_fat_pct;
    uint8_t metabolic_age;
} body_metrics_t;

// Callback for scale measurements
typedef void (*scale_measurement_callback_t)(const scale_measurement_t *measurement, const body_metrics_t *metrics);

/**
 * Initialize the BLE client for the Etekcity scale
 *
 * @param mac_address Scale MAC address string (e.g., "D0:4D:00:51:4F:8F")
 * @param callback Callback function for measurements
 * @return ESP_OK on success
 */
esp_err_t scale_ble_init(const char *mac_address, scale_measurement_callback_t callback);

/**
 * Start scanning for the scale
 *
 * @return ESP_OK on success
 */
esp_err_t scale_ble_start_scan(void);

/**
 * Stop BLE operations
 *
 * @return ESP_OK on success
 */
esp_err_t scale_ble_stop(void);

/**
 * Get current connection state
 *
 * @return true if connected to scale
 */
bool scale_ble_is_connected(void);

/**
 * Set user age for body metrics calculations
 *
 * @param age User age (10-120 years)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t scale_ble_set_user_age(int age);

/**
 * Set user height for body metrics calculations
 *
 * @param height_cm User height in centimeters (100-250cm)
 * @return ESP_OK on success, ESP_ERR_INVALID_ARG if out of range
 */
esp_err_t scale_ble_set_user_height(int height_cm);

/**
 * Set user sex for body metrics calculations
 *
 * @param is_male true for male, false for female
 * @return ESP_OK on success
 */
esp_err_t scale_ble_set_user_sex(bool is_male);

/**
 * Get current user profile
 *
 * @param age Pointer to store age (can be NULL)
 * @param height_cm Pointer to store height (can be NULL)
 * @param is_male Pointer to store sex (can be NULL)
 */
void scale_ble_get_user_profile(int *age, int *height_cm, bool *is_male);

#endif // SCALE_BLE_CLIENT_H
