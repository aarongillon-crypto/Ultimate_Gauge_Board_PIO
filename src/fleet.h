// ESP-NOW fleet sync: presence, remote commands, theme/config push.
//
// Protocol v1 (legacy, receive support kept forever): EspNowPacket, first byte
// type 1-8, broadcast only.
// Protocol v2: first byte magic 0xA7 (cannot collide with v1 types), then
// proto/type/flags. Adds named presence, full-theme push, behavior-config
// sync, and unicast commands. Mixed fleets are supported: presence dual-sends
// v1+v2 while any legacy peer is around, and pushes to legacy peers fall back
// to the v1 packet triple.
//
// The receive callback runs in the WiFi task on core 0 — it only stages data
// and sets flags; loop() applies and persists.
#pragma once
#include "app_state.h"

typedef struct __attribute__((packed)) {
    uint8_t type;
    int mode;
    uint32_t c1, c2, c3, c4;
    int value;
} EspNowPacket;

// Cache own MACs (call once after WiFi up) + esp_now init + recv callback.
void fleet_init();

// Guarded copy-out of the peer table (returns entry count).
int fleet_snapshot(PeerGauge *out, int max_entries);
extern int fleet_count;   // read-only convenience for status chips (atomic int)

// v1 broadcast primitives (still used for toggles + legacy fallback).
void broadcast_packet(EspNowPacket *pkt);
void broadcast_presence();                              // dual v1/v2 tick
void send_remote_command(uint8_t *targetMac, int newMode);

// v2 pushes. mac == nullptr → broadcast to all. Legacy peers (proto<2) get
// the v1 fallback automatically on unicast pushes.
void fleet_push_theme(const uint8_t *mac, uint8_t slot, bool activate);
void fleet_push_config(const uint8_t *mac);
void fleet_send_identify(const uint8_t *mac);

// Behavior config staged from a CONFIG_SYNC packet; consumed in loop().
bool fleet_take_pending_config(BehaviorConfig *out);
