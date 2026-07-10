// Definitions for the shared globals declared in app_state.h.
#include "app_state.h"

const char* MODE_NAMES[4] = { "BOOST", "AFR", "WATER", "OIL P" };

// Defaults reproduce the pre-config behavior EXACTLY (old RANGES table +
// hardcoded zone thresholds; WATER/OIL were always-mid → zones out of range).
const BehaviorConfig BEHAVIOR_DEFAULTS = {
  { { -15, 30,      0,   20   },     // BOOST: <0 low, <20 mid, else high
    {   8, 22,     10,   15   },     // AFR:   <10 low, <15 mid, else high
    {   0, 120, -9999, 9999   },     // WATER: always mid
    {   0, 100, -9999, 9999   } },   // OIL P: always mid
  0.24f,   // smoothing
  40.0f,   // max_rate units/sec
  30000    // peak_hold_ms
};
BehaviorConfig behavior = BEHAVIOR_DEFAULTS;

bool test_mode_enabled = false;
bool show_perf_stats = false;
bool peak_hold_enabled = true;
bool debug_mode_enabled = false;
GaugeMode current_mode = MODE_BOOST;
DisplayPage current_page = PAGE_GAUGE;
String device_name = "Gauge";
int current_brightness = 40;
uint8_t current_font = 0;
uint8_t secondary_metric = 0;
// 0=None 1=IAT 2=OilTemp 3=FuelTemp 4=FuelPress 5=TPS 6=EngLoad 7=IgnTiming 8=Baro 9=Speed 10=Gear
const char* SECONDARY_NAMES[] = {
  "None", "Intake Air Temp", "Oil Temp", "Fuel Temp", "Fuel Press",
  "TPS", "Eng Load", "Ign Timing", "Baro", "Speed", "Gear"
};
const int SECONDARY_COUNT = 11;

uint32_t text_color = 0xFFD700;
uint32_t color_low = 0x2196F3, color_mid = 0x4CAF50, color_high = 0xF44336;
uint32_t color_mode_label = 0x969696; // Mode label (gray)
uint32_t color_link_icon = 0x00C851;  // Connectivity icon (green)
uint32_t needle_color = 0xFF6600;     // Needle color (orange)
uint32_t color_peak = 0xFFFFFF;       // Peak stripe (white)
uint32_t color_background = 0x000000; // Screen background (black) — gradient stop 1
uint32_t color_background2 = 0x000000;
uint32_t color_background3 = 0x000000;
uint8_t  bg_grad_type = 0;
uint8_t  bg_grad_stops = 2;
uint16_t bg_grad_angle = 0;

GaugeTheme themes[THEME_SLOTS];
uint8_t active_theme = 0;
bool trimpot_theme_sync = false;
volatile int last_trimpot3 = -1;

float displayed_val = 0.0;
float target_val = 0.0;
float peak_val = -999.0;
float peak_low_val = 999.0;
unsigned long peak_timer = 0;

unsigned long perf_last_time = 0;
int perf_frames = 0;
int perf_fps = 0;
int perf_frame_ms = 0;
int perf_lvgl_ms = 0;

volatile bool flag_new_peer = false;
volatile uint32_t reboot_at_ms = 0;
volatile bool flag_theme_update = false;
volatile bool flag_bright_update = false;
volatile bool flag_stats_update = false;
volatile bool flag_page_update = false;
volatile bool flag_mode_update = false;
volatile int32_t pending_mode = -1;
volatile int32_t pending_brightness = -1;
volatile bool snap_displayed = false;
volatile uint32_t identify_end_ms = 0;
