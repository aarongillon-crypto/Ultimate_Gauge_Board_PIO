#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>

// BLE Service UUIDs (Generated unique UUIDs for this project)
#define BLE_SERVICE_GAUGE_DATA_UUID    "12340000-1234-1234-1234-123456789abc"
#define BLE_SERVICE_CONFIG_UUID        "12340001-1234-1234-1234-123456789abc"

// Gauge Data Characteristics (Read/Notify)
#define BLE_CHAR_CURRENT_VALUE_UUID    "12340010-1234-1234-1234-123456789abc"
#define BLE_CHAR_GAUGE_MODE_UUID       "12340011-1234-1234-1234-123456789abc"
#define BLE_CHAR_PEAK_VALUE_UUID       "12340012-1234-1234-1234-123456789abc"
#define BLE_CHAR_RPM_UUID              "12340013-1234-1234-1234-123456789abc"

// Configuration Characteristics (Read/Write)
#define BLE_CHAR_MODE_SETTING_UUID     "12340020-1234-1234-1234-123456789abc"
#define BLE_CHAR_TEXT_COLOR_UUID       "12340021-1234-1234-1234-123456789abc"
#define BLE_CHAR_COLOR_LOW_UUID        "12340022-1234-1234-1234-123456789abc"
#define BLE_CHAR_COLOR_MID_UUID        "12340023-1234-1234-1234-123456789abc"
#define BLE_CHAR_COLOR_HIGH_UUID       "12340024-1234-1234-1234-123456789abc"
#define BLE_CHAR_BRIGHTNESS_UUID       "12340025-1234-1234-1234-123456789abc"
#define BLE_CHAR_PEAK_HOLD_UUID        "12340026-1234-1234-1234-123456789abc"

// BLE Callback function types
typedef void (*BLEModeChangeCallback)(uint8_t newMode);
typedef void (*BLEColorChangeCallback)(uint32_t text, uint32_t low, uint32_t mid, uint32_t high);
typedef void (*BLEBrightnessChangeCallback)(uint8_t brightness);
typedef void (*BLEPeakHoldChangeCallback)(bool enabled);

// BLE Driver Functions
void ble_init(const char* deviceName);
void ble_update_gauge_value(float value);
void ble_update_gauge_mode(uint8_t mode);
void ble_update_peak_value(float value);
void ble_update_rpm(int rpm);
bool ble_is_connected();

// Register callbacks for configuration changes
void ble_register_mode_callback(BLEModeChangeCallback callback);
void ble_register_color_callback(BLEColorChangeCallback callback);
void ble_register_brightness_callback(BLEBrightnessChangeCallback callback);
void ble_register_peak_hold_callback(BLEPeakHoldChangeCallback callback);
