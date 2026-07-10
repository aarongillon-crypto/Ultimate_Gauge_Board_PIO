#include "Arduino.h"
#include "CANBus_Driver.h"
#include <stdio.h>

void canbus_init(void) {

  // Configure TWAI (CAN)
    twai_general_config_t g_config = TWAI_GENERAL_CONFIG_DEFAULT(CAN_TX_GPIO, CAN_RX_GPIO, TWAI_MODE_NORMAL);
    // Default rx_queue_len is 5 — far too shallow for a busy 1 Mbit Haltech bus.
    // Bursts overflowed it and silently dropped frames, including low-rate ones
    // like Rotary Trim 3 (0x3E4, 5 Hz) that drive theme-sync. Deepen the queue.
    g_config.rx_queue_len = 32;
    twai_timing_config_t t_config = TWAI_TIMING_CONFIG_1MBITS();
    twai_filter_config_t f_config = TWAI_FILTER_CONFIG_ACCEPT_ALL();  // Accept all IDs
 
    // Install and start TWAI driver
    if (twai_driver_install(&g_config, &t_config, &f_config) == ESP_OK) {
        Serial.println("TWAI driver installed.");
    } else {
        Serial.println("Failed to install TWAI driver.");
        while (1);
    }

    if (twai_start() == ESP_OK) {
        Serial.println("TWAI driver started. Listening for messages...");
    } else {
        Serial.println("Failed to start TWAI driver.");
        while (1);
    }
}

void canbus_recover(void) {
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