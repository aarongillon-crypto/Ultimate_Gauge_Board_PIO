#include "Arduino.h"
#include "CANBus_Driver.h"
#include <stdio.h>

// True once the TWAI driver is installed and started. A failed init no longer
// hangs boot (the old code did `while(1);`, bricking display/web/OTA until a
// power cycle) — the gauge comes up CAN-less and canbus_recover() retries.
bool canbus_ok = false;

// Selected bus speed. Default 1 Mbit (Haltech); set from NVS before canbus_init().
static uint32_t s_bitrate = 1000000;
void canbus_set_bitrate(uint32_t bps) { s_bitrate = bps; }

// Full (re)install attempt — used at boot and again from canbus_recover()
// if the driver never came up (e.g. transceiver fault at key-on).
static bool canbus_try_install(void) {
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_NORMAL);
    // Default rx_queue_len is 5 — far too shallow for a busy 1 Mbit Haltech bus.
    // Bursts overflowed it and silently dropped frames, including low-rate ones
    // like Rotary Trim 3 (0x3E4, 5 Hz) that drive theme-sync. Deepen the queue.
    g_config.rx_queue_len = 32;
    twai_timing_config_t t_config;
    switch (s_bitrate) {
        case 250000:  t_config = TWAI_TIMING_CONFIG_250KBITS(); break;  // some OEM buses
        case 500000:  t_config = TWAI_TIMING_CONFIG_500KBITS(); break;  // Evo/OEM powertrain
        default:      t_config = TWAI_TIMING_CONFIG_1MBITS();   break;  // Haltech (1 Mbit)
    }
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();  // Accept all IDs

    if (twai_driver_install(&g_config, &t_config, &f_config) != ESP_OK) return false;
    if (twai_start() != ESP_OK) {
        twai_driver_uninstall();
        return false;
    }
    return true;
}

void canbus_init(void) {
    canbus_ok = canbus_try_install();
    if (canbus_ok) {
        Serial.println("TWAI driver started. Listening for messages...");
    } else {
        Serial.println("TWAI init FAILED — CAN disabled; display/web/OTA stay up, will retry.");
    }
}

bool canbus_send(uint32_t id, const uint8_t* data, uint8_t len, bool extended) {
    if (!canbus_ok) return false;
    twai_message_t m = {};
    m.identifier = id;
    m.extd = extended ? 1 : 0;
    m.data_length_code = len > 8 ? 8 : len;
    for (int i = 0; i < m.data_length_code; i++) m.data[i] = data[i];
    return twai_transmit(&m, pdMS_TO_TICKS(5)) == ESP_OK;
}

void canbus_recover(void) {
    if (!canbus_ok) {
        // Driver never came up — retry a full install (caller rate-limits to 5s).
        canbus_ok = canbus_try_install();
        if (canbus_ok) Serial.println("TWAI recovered (late install).");
        return;
    }
    twai_status_info_t status;
    if (twai_get_status_info(&status) == ESP_OK) {
        if (status.state == TWAI_STATE_BUS_OFF) {
            Serial.println("TWAI bus-off detected, recovering...");
            twai_initiate_recovery();
            // Wait for recovery to complete (max 1s)
            for (int i = 0; i < 100; i++) {
                delay(10);
                twai_get_status_info(&status);
                if (status.state == TWAI_STATE_STOPPED) break;
            }
            twai_start();
        }
    }
}
