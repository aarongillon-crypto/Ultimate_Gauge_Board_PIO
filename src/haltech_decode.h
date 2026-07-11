// Generic Haltech CAN decode: walks the channel registry (haltech_channels.h)
// for every received frame and stores canonical float values per channel.
//
// Concurrency: values are written by ProcCAN as individual 32-bit stores
// (atomic on Xtensa) and read the same way — channels are independent, so no
// cross-field consistency is needed (unlike the old monolithic struct).
#pragma once
#include "app_state.h"
#include "haltech_channels.h"

// FreeRTOS task body (pinned in can_tasks_start).
void process_can_queue_task(void *arg);

// Canonical value of a channel by registry index (0.0 until first seen).
float haltech_value(int index);
// ms since the channel's frame was last seen (UINT32_MAX = never).
uint32_t haltech_age_ms(int index);
// Convenience: canonical value by stable chan key; 0.0/false when unknown.
float haltech_value_by_key(uint16_t key);
bool haltech_fresh_by_key(uint16_t key, uint32_t max_age_ms);

// Test mode: drive a handful of channels with synthetic sine data (canonical
// units), marking them fresh. Called from loop() when test_mode_enabled.
void haltech_test_inject();
