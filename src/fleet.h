// ESP-NOW fleet sync: presence, remote commands, theme/colour broadcasts.
// The receive callback runs in the WiFi task on core 0 — it only stages data
// and sets flags; loop() applies and persists (no NVS, no LVGL, no colour
// globals in callback context).
#pragma once
#include "app_state.h"

typedef struct __attribute__((packed)) {
    uint8_t type;
    int mode;
    uint32_t c1, c2, c3, c4;
    int value;
} EspNowPacket;

// Cache own MACs (call once after WiFi up) + esp_now init + recv callback.
// Web/OTA must already be running — fleet init failure is non-fatal.
void fleet_init();

// Guarded copy-out of the peer table (returns entry count).
int fleet_snapshot(PeerGauge *out, int max_entries);
extern int fleet_count;   // read-only convenience for status chips (atomic int)

void broadcast_packet(EspNowPacket *pkt);
void broadcast_presence();
void send_remote_command(uint8_t *targetMac, int newMode);
