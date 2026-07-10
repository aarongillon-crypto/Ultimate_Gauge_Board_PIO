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
// Safe-mode: zero the gradient type on the legacy live key AND the slot key.
void cfg_disable_gradient(uint8_t slot);

// Crash-safe boot flag.
void cfg_mark_boot_started();  // bootok=false — set before risky rendering
void cfg_mark_boot_ok();       // bootok=true  — set once rendered + backlit
