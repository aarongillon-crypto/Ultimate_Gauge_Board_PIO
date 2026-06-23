#pragma once
#include "driver/twai.h"

#define CAN_TX_GPIO     (gpio_num_t)5
#define CAN_RX_GPIO     (gpio_num_t)4

// Bus runs at 1 Mbit — set in CANBus_Driver.cpp via TWAI_TIMING_CONFIG_1MBITS()

void canbus_init();
void canbus_recover();