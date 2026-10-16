// The ONLY module that touches NVS (Preferences). Every persisted key in the
// firmware lives behind this API, so the "no NVS writes outside loopTask"
// rule is enforced in exactly one place (cfg_set_loop_task + guard).
#pragma once
#include "app_state.h"

// Capture the calling task (Arduino loopTask — call from setup()) as the only
// task allowed to write NVS. Writes from any other task log a loud [CFG][BUG]
// line instead of crashing the Wi-Fi task like the old in-callback writes did.
void cfg_set_loop_task();

// Load every persisted key into the app_state globals. defaultName is used
// when no device name is stored. Returns the stored "bootok" flag (false =
// previous boot crashed before completing — caller enters gradient safe mode).
bool cfg_load_all(const char* defaultName);
uint32_t cfg_peek_can_bitrate();   // early NVS read: CAN speed before cfg_load_all()

// Typed single-key writers (open/write/close per call).
void cfg_put_int(const char* key, int v);
void cfg_put_uint(const char* key, uint32_t v);
void cfg_put_uchar(const char* key, uint8_t v);
void cfg_put_ushort(const char* key, uint16_t v);
void cfg_put_bool(const char* key, bool v);
void cfg_put_string(const char* key, const String& v);

// Theme-slot persistence (keys t{slot}_xx — schema owned here).
void cfg_persist_theme_slot(uint8_t slot, const GaugeTheme& t);
void cfg_load_theme_slot(uint8_t slot, const GaugeTheme& dflt, GaugeTheme* out);
void cfg_put_theme_name(uint8_t slot, const String& name);      // key t{slot}_nm
String cfg_load_theme_name(uint8_t slot, const char* dflt);
// Safe-mode: zero the gradient type on the legacy live key AND the slot key.
void cfg_disable_gradient(uint8_t slot);

// Gauge behavior config (keys m{0-3}_min/max/z1/z2, smooth, maxrate, pkms).
void cfg_persist_behavior(const BehaviorConfig& b);
void cfg_load_behavior(BehaviorConfig* out);   // missing keys -> BEHAVIOR_DEFAULTS

// Crash-safe boot flag.
void cfg_mark_boot_started();  // bootok=false — set before risky rendering
void cfg_mark_boot_ok();       // bootok=true  — set once rendered + backlit

// --- Factory reset ---------------------------------------------------------
// Erase every key in the "gauge" namespace so the next boot comes up on the
// compiled-in defaults: themes, behavior (metric slots/ranges/zones), units,
// brightness, CAN speed, the layout-enable flag — everything persisted.
//
// keep_identity preserves the two keys that decide how you REACH the gauge:
// "devname" (the AP SSID) and "appass" (the WPA2 passphrase). A reset asked for
// over WiFi therefore doesn't drop the client that asked for it. Pass false for
// a true out-of-the-box device (AP reverts to OPEN, named from the chip ID).
//
// Every cfg_put_*/cfg_persist_* in this module becomes a NO-OP afterwards,
// until the device reboots. That is deliberate, not a side effect: an ESP-NOW
// packet from another gauge in the fleet (mode, brightness, a whole theme slot)
// lands in loop() and persists, so without the lock a fleet member could
// re-seed the namespace we just cleared during the few hundred ms between the
// HTTP response and ESP.restart().
void cfg_factory_reset(bool keep_identity);
