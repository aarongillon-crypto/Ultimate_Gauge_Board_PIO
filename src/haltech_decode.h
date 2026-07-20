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

// --- non-Haltech CAN sniffer (debug) ---
// Captures the most recent frame for each distinct CAN ID that is NOT in the
// Haltech registry (GlowCraft strips 0x500+, the 0x510 signals frame, and any
// other traffic on the bus). Debug aid for reverse-engineering third-party
// frames — e.g. discovering which ID/byte/bit a GlowCraft signal actually uses.
// Written by ProcCAN, read by the web task; a torn multi-byte read is possible
// and harmless for a diagnostic view.
struct CanSniffSlot {
  uint16_t id;
  uint8_t  dlc;
  uint8_t  ext;        // 1 = extended (29-bit) ID
  uint8_t  data[8];
  uint32_t last_ms;    // millis() of last capture
  uint32_t count;      // total frames seen for this ID (0 = empty slot)
};
// Returns a pointer to the internal table and writes its length to *count.
// Iterate all entries; skip those with .count == 0 (unused slots).
const CanSniffSlot* cansniff_table(int* count);

// Human-readable label for a known CAN ID, or nullptr if unknown. Returns starter
// labels for known foreign/OEM IDs (currently Mitsubishi Evo X CZ4A) so the sniffer
// isn't a wall of raw hex when the bus is switched to 500 kbit. Labels are
// community-sourced and UNVERIFIED — confirm on car. Harmless on the Haltech bus.
const char* can_label(uint16_t id);
