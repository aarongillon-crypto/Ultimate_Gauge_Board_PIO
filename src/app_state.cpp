// Definitions for the shared globals declared in app_state.h.
#include "app_state.h"

// Defaults reproduce the classic 4 modes, now bound to their V2-correct
// channels: BOOST = Manifold Pressure (0x360/2), AFR = Wideband 1 (0x368/0),
// WATER = Coolant Temperature (0x3E0/0 — was wrongly read from 0x362),
// OIL P = Oil Pressure (0x361/2). Ranges in display units (psi/°C/AFR).
const BehaviorConfig BEHAVIOR_DEFAULTS = {
  { { (uint16_t)((0x360 << 4) | 2), "BOOST", -15, 30,      0,   20   },  // <0 low, <20 mid, else high
    { (uint16_t)((0x368 << 4) | 0), "AFR",     8, 22,     10,   15   },  // <10 low, <15 mid, else high
    { (uint16_t)((0x3E0 << 4) | 0), "WATER",   0, 120, -9999, 9999   },  // always mid
    { (uint16_t)((0x361 << 4) | 2), "OIL P",   0, 100, -9999, 9999   } },// always mid
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
int dim_brightness = 20;
bool dim_can_enabled = false;
uint8_t dim_source = DIM_SRC_EITHER;
uint8_t current_font = 0;
uint16_t secondary_chan = 0;   // chan_key, 0 = none
uint32_t can_bitrate = 1000000;   // default Haltech 1 Mbit; overridden from NVS at boot

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

bool layout_enabled = true;
volatile bool flag_layout_reload = false;
volatile int32_t pending_layout_page = -1;
