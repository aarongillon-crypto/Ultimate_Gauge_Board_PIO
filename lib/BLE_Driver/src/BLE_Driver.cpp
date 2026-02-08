#include "BLE_Driver.h"

// BLE Objects
static NimBLEServer* pServer = nullptr;
static NimBLEService* pGaugeDataService = nullptr;
static NimBLEService* pConfigService = nullptr;

// Gauge Data Characteristics
static NimBLECharacteristic* pCharCurrentValue = nullptr;
static NimBLECharacteristic* pCharGaugeMode = nullptr;
static NimBLECharacteristic* pCharPeakValue = nullptr;
static NimBLECharacteristic* pCharRPM = nullptr;

// Configuration Characteristics
static NimBLECharacteristic* pCharModeSetting = nullptr;
static NimBLECharacteristic* pCharTextColor = nullptr;
static NimBLECharacteristic* pCharColorLow = nullptr;
static NimBLECharacteristic* pCharColorMid = nullptr;
static NimBLECharacteristic* pCharColorHigh = nullptr;
static NimBLECharacteristic* pCharBrightness = nullptr;
static NimBLECharacteristic* pCharPeakHold = nullptr;

// Connection tracking
static bool deviceConnected = false;
static bool oldDeviceConnected = false;

// Callback function pointers
static BLEModeChangeCallback modeChangeCallback = nullptr;
static BLEColorChangeCallback colorChangeCallback = nullptr;
static BLEBrightnessChangeCallback brightnessChangeCallback = nullptr;
static BLEPeakHoldChangeCallback peakHoldChangeCallback = nullptr;

// Server callbacks
class ServerCallbacks: public NimBLEServerCallbacks {
    void onConnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo) {
        deviceConnected = true;
        Serial.println("BLE Client Connected");
    }

    void onDisconnect(NimBLEServer* pServer, NimBLEConnInfo& connInfo, int reason) {
        deviceConnected = false;
        Serial.println("BLE Client Disconnected");
        // Start advertising again
        NimBLEDevice::startAdvertising();
    }
};

// Configuration Characteristic Callbacks
class ModeSettingCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        uint8_t* data = pCharacteristic->getData();
        if (data != nullptr && pCharacteristic->getDataLength() >= 1) {
            uint8_t newMode = data[0];
            Serial.printf("BLE Mode Change Request: %d\n", newMode);
            if (modeChangeCallback != nullptr) {
                modeChangeCallback(newMode);
            }
        }
    }
};

class ColorCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        if (colorChangeCallback != nullptr && pCharacteristic->getDataLength() >= 16) {
            uint8_t* data = pCharacteristic->getData();
            uint32_t textColor = (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
            uint32_t lowColor = (data[4] << 24) | (data[5] << 16) | (data[6] << 8) | data[7];
            uint32_t midColor = (data[8] << 24) | (data[9] << 16) | (data[10] << 8) | data[11];
            uint32_t highColor = (data[12] << 24) | (data[13] << 16) | (data[14] << 8) | data[15];
            Serial.println("BLE Color Change Request");
            colorChangeCallback(textColor, lowColor, midColor, highColor);
        }
    }
};

class BrightnessCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        uint8_t* data = pCharacteristic->getData();
        if (data != nullptr && pCharacteristic->getDataLength() >= 1) {
            uint8_t brightness = data[0];
            Serial.printf("BLE Brightness Change Request: %d\n", brightness);
            if (brightnessChangeCallback != nullptr) {
                brightnessChangeCallback(brightness);
            }
        }
    }
};

class PeakHoldCallbacks: public NimBLECharacteristicCallbacks {
    void onWrite(NimBLECharacteristic* pCharacteristic, NimBLEConnInfo& connInfo) {
        uint8_t* data = pCharacteristic->getData();
        if (data != nullptr && pCharacteristic->getDataLength() >= 1) {
            bool enabled = (data[0] != 0);
            Serial.printf("BLE Peak Hold Change Request: %d\n", enabled);
            if (peakHoldChangeCallback != nullptr) {
                peakHoldChangeCallback(enabled);
            }
        }
    }
};

void ble_init(const char* deviceName) {
    Serial.println("Initializing BLE...");

    // Initialize NimBLE
    NimBLEDevice::init(deviceName);
    NimBLEDevice::setPower(ESP_PWR_LVL_P9); // Max power

    // Create BLE Server
    pServer = NimBLEDevice::createServer();
    pServer->setCallbacks(new ServerCallbacks());

    // Create Gauge Data Service
    pGaugeDataService = pServer->createService(BLE_SERVICE_GAUGE_DATA_UUID);

    // Current Value (Float, Read/Notify)
    pCharCurrentValue = pGaugeDataService->createCharacteristic(
        BLE_CHAR_CURRENT_VALUE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Gauge Mode (uint8, Read/Notify)
    pCharGaugeMode = pGaugeDataService->createCharacteristic(
        BLE_CHAR_GAUGE_MODE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // Peak Value (Float, Read/Notify)
    pCharPeakValue = pGaugeDataService->createCharacteristic(
        BLE_CHAR_PEAK_VALUE_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    // RPM (int16, Read/Notify)
    pCharRPM = pGaugeDataService->createCharacteristic(
        BLE_CHAR_RPM_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::NOTIFY
    );

    pGaugeDataService->start();

    // Create Configuration Service
    pConfigService = pServer->createService(BLE_SERVICE_CONFIG_UUID);

    // Mode Setting (uint8, Read/Write)
    pCharModeSetting = pConfigService->createCharacteristic(
        BLE_CHAR_MODE_SETTING_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCharModeSetting->setCallbacks(new ModeSettingCallbacks());

    // Text Color (uint32, Read/Write) - Note: BLE sends as 4 bytes
    pCharTextColor = pConfigService->createCharacteristic(
        BLE_CHAR_TEXT_COLOR_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCharTextColor->setCallbacks(new ColorCallbacks());

    // Brightness (uint8, Read/Write)
    pCharBrightness = pConfigService->createCharacteristic(
        BLE_CHAR_BRIGHTNESS_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCharBrightness->setCallbacks(new BrightnessCallbacks());

    // Peak Hold Enable (bool/uint8, Read/Write)
    pCharPeakHold = pConfigService->createCharacteristic(
        BLE_CHAR_PEAK_HOLD_UUID,
        NIMBLE_PROPERTY::READ | NIMBLE_PROPERTY::WRITE
    );
    pCharPeakHold->setCallbacks(new PeakHoldCallbacks());

    pConfigService->start();

    // Start advertising
    NimBLEAdvertising* pAdvertising = NimBLEDevice::getAdvertising();
    pAdvertising->addServiceUUID(BLE_SERVICE_GAUGE_DATA_UUID);
    pAdvertising->addServiceUUID(BLE_SERVICE_CONFIG_UUID);
    pAdvertising->setScanResponse(true);
    pAdvertising->setMinPreferred(0x06);  // Functions that help with iPhone connections issue
    pAdvertising->setMaxPreferred(0x12);
    NimBLEDevice::startAdvertising();

    Serial.println("BLE Initialized and Advertising");
}

void ble_update_gauge_value(float value) {
    if (pCharCurrentValue != nullptr) {
        pCharCurrentValue->setValue(value);
        if (deviceConnected) {
            pCharCurrentValue->notify();
        }
    }
}

void ble_update_gauge_mode(uint8_t mode) {
    if (pCharGaugeMode != nullptr) {
        pCharGaugeMode->setValue(&mode, 1);
        if (deviceConnected) {
            pCharGaugeMode->notify();
        }
    }
}

void ble_update_peak_value(float value) {
    if (pCharPeakValue != nullptr) {
        pCharPeakValue->setValue(value);
        if (deviceConnected) {
            pCharPeakValue->notify();
        }
    }
}

void ble_update_rpm(int rpm) {
    if (pCharRPM != nullptr) {
        int16_t rpmValue = (int16_t)rpm;
        pCharRPM->setValue((uint8_t*)&rpmValue, 2);
        if (deviceConnected) {
            pCharRPM->notify();
        }
    }
}

bool ble_is_connected() {
    return deviceConnected;
}

// Register callback functions
void ble_register_mode_callback(BLEModeChangeCallback callback) {
    modeChangeCallback = callback;
}

void ble_register_color_callback(BLEColorChangeCallback callback) {
    colorChangeCallback = callback;
}

void ble_register_brightness_callback(BLEBrightnessChangeCallback callback) {
    brightnessChangeCallback = callback;
}

void ble_register_peak_hold_callback(BLEPeakHoldChangeCallback callback) {
    peakHoldChangeCallback = callback;
}
