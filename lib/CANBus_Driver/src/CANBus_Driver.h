#pragma once
#include "driver/twai.h"

#define CAN_TX_GPIO     (gpio_num_t)5
#define CAN_RX_GPIO     (gpio_num_t)4

// Bus runs at 1 Mbit — set in CANBus_Driver.cpp via TWAI_TIMING_CONFIG_1MBITS()

void canbus_init();
void canbus_recover();

// Transmit one standard (11-bit) or extended frame. Non-blocking-ish: waits up
// to 5 ms for a TX mailbox. Returns false if CAN is down or the queue is full.
// The bus is installed in NORMAL mode, so the gauge can both RX and TX.
bool canbus_send(uint32_t id, const uint8_t* data, uint8_t len, bool extended = false);

// False if the TWAI driver failed to install/start (dead transceiver, etc.).
// The gauge keeps running without CAN; canbus_recover() retries the install.
extern bool canbus_ok;