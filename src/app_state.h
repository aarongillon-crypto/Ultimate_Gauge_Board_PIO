// Shared application state: types, enums, and the live globals every module
// sees. Ownership rule (see docs/ARCHITECTURE note in plan): loopTask owns all
// LVGL objects, all live colour/config globals, and all NVS access. Cross-task
// writers (ESP-NOW callback on core 0, ProcCAN) go through staging + flags only.
#pragma once
#include <Arduino.h>

// --- FIRMWARE VERSION ---
// Bump FIRMWARE_VERSION on each release. FIRMWARE_BUILD is stamped automatically
// by the compiler every build, so it always changes even if the version is not
// bumped -- use it to confirm an OTA upload actually took effect.
#define FIRMWARE_VERSION "2.1.0"
#define FIRMWARE_VER_MAJOR 2
#define FIRMWARE_VER_MINOR 1
#define FIRMWARE_VER_PATCH 0
#define FIRMWARE_BUILD   __DATE__ " " __TIME__

// --- MODES / PAGES ---
enum GaugeMode { MODE_BOOST=0, MODE_AFR=1, MODE_WATER=2, MODE_OIL=3 };
enum DisplayPage { PAGE_GAUGE = 0, PAGE_GLOWCRAFT = 1 };

// --- GAUGE BEHAVIOR CONFIG (per-mode channel + ranges/zones + dynamics) ---
// Each of the 4 mode slots binds to ANY channel from the Haltech V2 registry
// (haltech_channels.h) via its stable chan_key, with a custom display label.
// Ranges/zones are in DISPLAY units (what the dial shows). z1/z2 outside
// [min,max] = single-colour gauge. Defaults reproduce the classic 4 modes.
struct ModeConfig  { uint16_t chan_key; // registry key: (can_id<<4)|byte_offset
                     char label[14];    // gauge face label, e.g. "BOOST"
                     float min, max;    // gauge sweep range (display units)
                     float z1, z2; };   // colour zones: v<z1 low, v<z2 mid, else high
struct BehaviorConfig {
  ModeConfig mode[4];
  float smoothing;        // needle smoothing factor (default 0.24)
  float max_rate;         // max displayed change, units/sec (default 40)
  uint32_t peak_hold_ms;  // peak reset window (default 30000)
};
extern BehaviorConfig behavior;
extern const BehaviorConfig BEHAVIOR_DEFAULTS;

// Live CAN data lives in the channel value store (haltech_decode.h), indexed
// by the Haltech V2 channel registry — the old fixed HaltechData_t is gone.

// --- THEME SLOTS ---
struct GaugeTheme {
  uint32_t text, low, mid, high;                              // Dynamic Elements
  uint32_t background, mode_label, link_icon, needle, peak;   // Static Elements
  uint32_t bg_grad2, bg_grad3;                                // Background gradient stops 2 & 3
  uint8_t  bg_grad_type;                                      // 0=solid 1=linV 2=linH 3=linAngle 4=radial 5=conical
  uint8_t  bg_grad_stops;                                     // 2 or 3
  uint16_t bg_grad_angle;                                     // degrees (linAngle dir / conical start)
};
#define THEME_SLOTS 4

// --- FLEET ---
typedef struct {
  uint8_t mac[6];
  int mode;
  unsigned long last_seen;
  uint8_t proto;          // 1 = legacy v1 packets only, 2 = speaks protocol v2
  uint8_t active_theme;   // v2 presence only
  uint8_t fw[3];          // v2 presence only: major/minor/patch
  char name[21];          // v2 presence only ("" for legacy peers)
} PeerGauge;

// --- CONFIG GLOBALS (owned by loopTask; loaded from NVS at boot) ---
extern bool test_mode_enabled;
extern bool show_perf_stats;
extern bool peak_hold_enabled;
extern bool debug_mode_enabled;
extern GaugeMode current_mode;
extern DisplayPage current_page;
extern String device_name;              // used for AP SSID
extern int current_brightness;
extern uint8_t current_font;
extern uint16_t secondary_chan;   // chan_key of the secondary readout (0 = none)

// --- LIVE COLOURS (owned by loopTask) ---
extern uint32_t text_color;
extern uint32_t color_low, color_mid, color_high;
extern uint32_t color_mode_label;
extern uint32_t color_link_icon;
extern uint32_t needle_color;
extern uint32_t color_peak;
extern uint32_t color_background;       // gradient stop 1 / solid fill
extern uint32_t color_background2;      // gradient stop 2
extern uint32_t color_background3;      // gradient stop 3 (when bg_grad_stops==3)
extern uint8_t  bg_grad_type;           // 0=solid 1=lin-V 2=lin-H 3=lin-angle 4=radial 5=conical
extern uint8_t  bg_grad_stops;          // 2 or 3
extern uint16_t bg_grad_angle;          // degrees: lin-angle direction / conical start

extern GaugeTheme themes[THEME_SLOTS];
extern uint8_t active_theme;            // live slot (rotary-driven when sync is on)
extern bool trimpot_theme_sync;         // NVS "tpsync" — gate the rotary->theme link
extern volatile int last_trimpot3;      // last decoded rotary position (-1 = unseen)

// --- GAUGE VALUE STATE (written by gauge page; peaks reset by web handler) ---
extern float displayed_val;
extern float target_val;
extern float peak_val;
extern float peak_low_val;
extern unsigned long peak_timer;

// --- PERF STATS ---
extern unsigned long perf_last_time;
extern int perf_frames;
extern int perf_fps;
extern int perf_frame_ms;
extern int perf_lvgl_ms;

// --- CROSS-TASK FLAGS & PENDING VALUES ---
// Flags are set anywhere, consumed ONLY in loop(). Pendings carry the payload
// for changes arriving from the ESP-NOW callback (-1 = nothing pending); loop
// applies AND persists them (no NVS in callback context).
extern volatile bool flag_new_peer;
extern volatile uint32_t reboot_at_ms;  // deferred reboot deadline (0 = none)
extern volatile bool flag_theme_update;
extern volatile bool flag_bright_update;
extern volatile bool flag_stats_update;
extern volatile bool flag_page_update;
extern volatile bool flag_mode_update;
extern volatile int32_t pending_mode;
extern volatile int32_t pending_brightness;
extern volatile bool snap_displayed;    // snap needle/value to target on next frame
extern volatile uint32_t identify_end_ms;  // fleet "identify" backlight blink deadline (0 = off)
