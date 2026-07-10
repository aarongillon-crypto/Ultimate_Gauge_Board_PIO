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
#define FIRMWARE_VERSION "1.2.0"
#define FIRMWARE_BUILD   __DATE__ " " __TIME__

// --- MODES / PAGES ---
enum GaugeMode { MODE_BOOST=0, MODE_AFR=1, MODE_WATER=2, MODE_OIL=3 };
enum DisplayPage { PAGE_GAUGE = 0, PAGE_GLOWCRAFT = 1 };

extern const float RANGES[4][2];
extern const char* MODE_NAMES[4];

// --- LIVE CAN DATA ---
typedef struct {
  float boost_psi; float afr_gas; int rpm; int water_temp_c; float oil_press_psi;
  int intake_air_temp_c; float oil_temp_c; float fuel_press_psi; int tps_percent;
  int engine_load_pct; float ign_timing_deg; float baro_kpa; float fuel_temp_c;
  float vehicle_speed_kph; int8_t gear;
} HaltechData_t;

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
typedef struct { uint8_t mac[6]; int mode; unsigned long last_seen; } PeerGauge;

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
extern uint8_t secondary_metric;
extern const char* SECONDARY_NAMES[];
extern const int SECONDARY_COUNT;

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
extern const unsigned long PEAK_HOLD_TIME;

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
