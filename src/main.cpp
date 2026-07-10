#include <Arduino.h>
#include "CANBus_Driver.h"
#include "GlowCraft_Driver.h"
#include "LVGL_Driver.h"
#include "I2C_Driver.h"
#include "Display_ST7701.h"
#include "TCA9554PWR.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"
#include <WiFi.h>
#include <WebServer.h>
#include <Preferences.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <esp_system.h>
#include <ArduinoOTA.h>
#include <Update.h>
#include <math.h>

// --- FIRMWARE VERSION ---
// Bump FIRMWARE_VERSION on each release. FIRMWARE_BUILD is stamped automatically
// by the compiler every build, so it always changes even if the version is not
// bumped -- use it to confirm an OTA upload actually took effect.
#define FIRMWARE_VERSION "1.1.1"
#define FIRMWARE_BUILD   __DATE__ " " __TIME__

// --- CONFIGURATION ---
bool test_mode_enabled = false;
bool show_perf_stats = false;
bool peak_hold_enabled = true;
bool debug_mode_enabled = false;

unsigned long debug_last_print = 0;
#define DEBUG_INTERVAL_MS 5000  // print every 5s when debug enabled

static const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_SW:        return "SOFTWARE (ESP.restart)";
    case ESP_RST_PANIC:     return "PANIC/EXCEPTION";
    case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

enum GaugeMode { MODE_BOOST=0, MODE_AFR=1, MODE_WATER=2, MODE_OIL=3 };

LV_FONT_DECLARE(dseg14_96);
LV_FONT_DECLARE(dseg14_120);

#define FONT_FIRAMONO_AVAILABLE 1
#if FONT_FIRAMONO_AVAILABLE
LV_FONT_DECLARE(firamono_96);
LV_FONT_DECLARE(firamono_120);
#endif

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

typedef struct {
  float boost_psi; float afr_gas; int rpm; int water_temp_c; float oil_press_psi;
  int intake_air_temp_c; float oil_temp_c; float fuel_press_psi; int tps_percent;
  int engine_load_pct; float ign_timing_deg; float baro_kpa; float fuel_temp_c;
  float vehicle_speed_kph; int8_t gear;
} HaltechData_t;
HaltechData_t HaltechData;

Preferences preferences;
WebServer server(80);
GaugeMode current_mode = MODE_BOOST; 

String device_name = "Gauge";  // loaded from NVS, used for AP SSID
uint32_t text_color = 0xFFD700;
uint32_t color_low = 0x2196F3, color_mid = 0x4CAF50, color_high = 0xF44336;
uint32_t color_mode_label = 0x969696; // Mode label (gray)
uint32_t color_link_icon = 0x00C851; // Connectivity icon (green)
uint32_t needle_color = 0xFF6600;     // Needle color (orange)
uint32_t color_peak = 0xFFFFFF;      // Peak stripe (white)
uint32_t color_background = 0x000000; // Screen background (black) — gradient stop 1
// Background gradient (stop 1 = color_background). 0=solid keeps the flat fill.
uint32_t color_background2 = 0x000000; // gradient stop 2
uint32_t color_background3 = 0x000000; // gradient stop 3 (used when bg_grad_stops==3)
uint8_t  bg_grad_type = 0;             // 0=solid 1=lin-V 2=lin-H 3=lin-angle 4=radial 5=conical
uint8_t  bg_grad_stops = 2;            // 2 or 3
uint16_t bg_grad_angle = 0;            // degrees: lin-angle direction / conical start
uint32_t current_applied_text = 0;
int current_brightness = 40;
uint8_t current_font = 0;

// --- TRIMPOT-DRIVEN THEME SLOTS ---
// Rotary Trim 3 (Haltech 0x3E4 byte 6, values 0-3) can select one of four full
// colour palettes live. Each slot holds the same 9 colours the two web theme
// forms manage, so "edit the active slot" works through the existing pickers.
struct GaugeTheme {
  uint32_t text, low, mid, high;                              // Dynamic Elements
  uint32_t background, mode_label, link_icon, needle, peak;   // Static Elements
  uint32_t bg_grad2, bg_grad3;                                // Background gradient stops 2 & 3
  uint8_t  bg_grad_type;                                      // 0=solid 1=linV 2=linH 3=linAngle 4=radial 5=conical
  uint8_t  bg_grad_stops;                                     // 2 or 3
  uint16_t bg_grad_angle;                                     // degrees (linAngle dir / conical start)
};
#define THEME_SLOTS 4
GaugeTheme themes[THEME_SLOTS];
uint8_t active_theme = 0;             // live slot (rotary-driven when sync is on)
bool trimpot_theme_sync = false;      // NVS "tpsync" — gate the rotary->theme link
volatile int last_trimpot3 = -1;      // last decoded rotary position (-1 = unseen)

// Distinct factory palettes for slots 1-3 (slot 0 inherits the existing theme).
static const GaugeTheme THEME_DEFAULTS[THEME_SLOTS] = {
  // slot 0 — placeholder; overwritten by the live/legacy theme at load time (solid bg)
  {0xFFD700,0x2196F3,0x4CAF50,0xF44336, 0x000000,0x969696,0x00C851,0xFF6600,0xFFFFFF, 0x000000,0x000000, 0,2,0},
  // slot 1 — "Street" cool blue: vertical fade to black
  {0xFFFFFF,0x2196F3,0x4CAF50,0xF44336, 0x001830,0x6699BB,0x00C851,0x33AAFF,0xFFFFFF, 0x000000,0x000000, 1,2,0},
  // slot 2 — "Sport" amber: radial glow from centre
  {0xFFD700,0x00C8FF,0xFFC107,0xFF3B30, 0x1A1000,0xAA8844,0xFFAA00,0xFF8A00,0xFFFFFF, 0x000000,0x000000, 4,2,0},
  // slot 3 — "Race" red: 3-stop linear at 45°
  {0xFFFFFF,0x00E676,0xFFEB3B,0xFF1744, 0x200000,0xBB5555,0xFF3B30,0xFF1744,0xFFFFFF, 0x080000,0x000000, 3,3,45},
};
void theme_to_globals(uint8_t i);
void globals_to_theme(uint8_t i);
void persist_theme(uint8_t i);
void load_all_themes();

// Forward declarations

float displayed_val = 0.0;
float target_val = 0.0;
float peak_val = -999.0;
float peak_low_val = 999.0;
unsigned long peak_timer = 0;
const unsigned long PEAK_HOLD_TIME = 30000;
unsigned long last_data_time = 0;
unsigned long last_broadcast = 0;

// Secondary metric (shown centred in peak-label area when peak hold is OFF)
uint8_t secondary_metric = 0;
// 0=None 1=IAT 2=OilTemp 3=FuelTemp 4=FuelPress 5=TPS 6=EngLoad 7=IgnTiming 8=Baro 9=Speed 10=Gear
const char* SECONDARY_NAMES[] = {
  "None", "Intake Air Temp", "Oil Temp", "Fuel Temp", "Fuel Press",
  "TPS", "Eng Load", "Ign Timing", "Baro", "Speed", "Gear"
};
const int SECONDARY_COUNT = 11;

// PERF STATS
unsigned long perf_last_time = 0;
int perf_frames = 0;
int perf_fps = 0;
int perf_frame_ms = 0;
int perf_lvgl_ms = 0;

volatile bool flag_new_peer = false;
// Deferred reboot deadline (0 = none). Handlers set this instead of calling
// delay()+ESP.restart() inline, so the HTTP response actually flushes and the
// LVGL loop is never blocked waiting on a reboot.
volatile uint32_t reboot_at_ms = 0;
volatile bool flag_theme_update = false; 
volatile bool flag_bright_update = false;
volatile bool flag_stats_update = false;
volatile bool flag_page_update = false;
volatile bool flag_mode_update = false;   // apply a live gauge-mode change (no restart)
volatile bool flag_persist_theme = false; // persist the live theme to NVS from loop() — deferred out of the ESP-NOW recv callback, where blocking flash writes crash the Wi-Fi task
// Remote mode/brightness arrive in the ESP-NOW callback but are applied AND
// persisted from loop() (-1 = nothing pending) — same no-NVS-in-callback rule.
volatile int32_t pending_mode = -1;
volatile int32_t pending_brightness = -1;
// Own MACs, cached once in setup_wifi() so the recv callback doesn't call into
// the WiFi driver per packet just to filter its own echoes.
static uint8_t my_sta_mac[6] = {0};
static uint8_t my_ap_mac[6]  = {0};
volatile bool snap_displayed = false;      // snap needle/value to target on next frame

#define WIFI_CHANNEL 1
typedef struct __attribute__((packed)) { 
    uint8_t type; 
    int mode; 
    uint32_t c1, c2, c3, c4; 
    int value; 
} EspNowPacket;

typedef struct { uint8_t mac[6]; int mode; unsigned long last_seen; } PeerGauge;
PeerGauge fleet[10]; int fleet_count = 0;
// fleet[] is written from the ESP-NOW callback (WiFi task, core 0) and read by
// web handlers in loop() — guard both sides with a short critical section.
static portMUX_TYPE fleet_mux = portMUX_INITIALIZER_UNLOCKED;
// Copy out a consistent view of the fleet for readers (returns entry count).
int fleet_snapshot(PeerGauge *out, int max_entries) {
  taskENTER_CRITICAL(&fleet_mux);
  int n = (fleet_count < max_entries) ? fleet_count : max_entries;
  memcpy(out, fleet, n * sizeof(PeerGauge));
  taskEXIT_CRITICAL(&fleet_mux);
  return n;
}

lv_obj_t *val_label_int;
lv_obj_t *val_label_dec;
lv_obj_t *mode_label;
lv_obj_t *link_icon; 
lv_obj_t *bar; lv_obj_t *peak_dot;
lv_obj_t *peak_high_label; lv_obj_t *peak_low_label;
lv_obj_t *perf_label;
lv_obj_t *needle_tip;

// --- DISPLAY PAGES ---
// The gauge lives on its own screen (gauge_scr); the GlowCraft LED view is a
// second screen. current_page selects which is loaded and which update path runs.
enum DisplayPage { PAGE_GAUGE = 0, PAGE_GLOWCRAFT = 1 };
DisplayPage current_page = PAGE_GAUGE;
lv_obj_t *gauge_scr = nullptr;      // default screen, holds the gauge UI
lv_obj_t *glowcraft_scr = nullptr;  // holds the LED silhouette view

const float RANGES[4][2] = { {-15,30}, {8,22}, {0,120}, {0,100} };
const char* MODE_NAMES[4] = { "BOOST", "AFR", "WATER", "OIL P" };

void drivers_init() {
  i2c_init(); tca9554pwr_init(0x00); lcd_init(); canbus_init(); glowcraft_init(); lvgl_init();
}

void update_peer_list(const uint8_t *mac, int mode) {
  bool added = false;
  taskENTER_CRITICAL(&fleet_mux);
  bool found = false;
  for (int i = 0; i < fleet_count; i++) {
    if (memcmp(fleet[i].mac, mac, 6) == 0) {
      fleet[i].mode = mode; fleet[i].last_seen = millis();
      found = true; break;
    }
  }
  if (!found && fleet_count < 10) {
    memcpy(fleet[fleet_count].mac, mac, 6);
    fleet[fleet_count].mode = mode; fleet[fleet_count].last_seen = millis();
    fleet_count++;
    added = true;
  }
  taskEXIT_CRITICAL(&fleet_mux);
  if (added) flag_new_peer = true;
}

void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t *incomingData, int len) {
  const uint8_t* mac = info->src_addr;
  if (len != sizeof(EspNowPacket)) return;

  // Ignore our own packets echoed back via the AP interface (MACs cached at setup)
  if (memcmp(mac, my_sta_mac, 6) == 0 || memcmp(mac, my_ap_mac, 6) == 0) return;

  EspNowPacket *pkt = (EspNowPacket *)incomingData;

  if (debug_mode_enabled) {
    Serial.printf("[ESP-NOW] type=%d mode=%d from %02X:%02X:%02X:%02X:%02X:%02X to %02X:%02X:%02X:%02X:%02X:%02X\n",
      pkt->type, pkt->mode,
      mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
      info->des_addr[0], info->des_addr[1], info->des_addr[2],
      info->des_addr[3], info->des_addr[4], info->des_addr[5]);
  }

  if (pkt->type == 1) {
    update_peer_list(mac, pkt->mode);
  }
  else if (pkt->type == 2) {
    // Remote mode change — applied AND persisted from loop() (no NVS here).
    pending_mode = constrain(pkt->mode, 0, 3);
    flag_mode_update = true;
  }
  else if (pkt->type == 3) {
    text_color = pkt->c1; color_low = pkt->c2; color_mid = pkt->c3; color_high = pkt->c4;
    // Set globals only. The NVS writes + slot capture are deferred to loop() via
    // flag_persist_theme — doing blocking flash writes here (Wi-Fi task context)
    // crashes/hangs the receiver, so a broadcast "Apply to ALL" never landed.
    flag_theme_update = true; flag_persist_theme = true;
  }
  else if (pkt->type == 4) {
    test_mode_enabled = (pkt->value == 1);
  }
  else if (pkt->type == 5) {
    // Remote brightness — applied AND persisted from loop() (no NVS here).
    pending_brightness = constrain(pkt->value, 10, 100);
    flag_bright_update = true;
  }
  else if (pkt->type == 6) { 
    show_perf_stats = (pkt->value == 1);
    flag_stats_update = true;
  }
  else if (pkt->type == 7) {
    // UI Colors broadcast. Globals only — persistence deferred to loop() (see type 3).
    color_background = pkt->c1;
    color_mode_label = pkt->c2;
    color_link_icon = pkt->c3;
    needle_color = pkt->c4;
    color_peak = (uint32_t)pkt->value;
    flag_theme_update = true; flag_persist_theme = true;
  }
  else if (pkt->type == 8) {
    // Background gradient broadcast: c1/c2 = stops 2/3, c3 = background (stop 1),
    // value packs type|stops|angle (see handleGrad). Persistence deferred to loop().
    color_background2 = pkt->c1;
    color_background3 = pkt->c2;
    color_background  = pkt->c3;
    uint32_t v = (uint32_t)pkt->value;
    bg_grad_type  = (uint8_t)(v & 0x0F);
    bg_grad_stops = (uint8_t)((v >> 4) & 0x0F);
    bg_grad_angle = (uint16_t)((v >> 8) & 0xFFFF);
    flag_theme_update = true; flag_persist_theme = true;
  }

}

void broadcast_packet(EspNowPacket *pkt) {
  uint8_t broadcastAddress[] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, broadcastAddress, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_AP; 
  if (!esp_now_is_peer_exist(broadcastAddress)) esp_now_add_peer(&peerInfo);
  esp_now_send(broadcastAddress, (uint8_t *) pkt, sizeof(EspNowPacket));
}

void broadcast_presence() {
  EspNowPacket pkt = {}; pkt.type = 1; pkt.mode = current_mode;
  broadcast_packet(&pkt);
}

void send_remote_command(uint8_t *targetMac, int newMode) {
  EspNowPacket pkt = {}; pkt.type = 2; pkt.mode = newMode;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, targetMac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_AP;
  if (!esp_now_is_peer_exist(targetMac)) esp_now_add_peer(&peerInfo);
  esp_now_send(targetMac, (uint8_t *) &pkt, sizeof(pkt));
}



String colorToHex(uint32_t color) {
  char buf[8]; snprintf(buf, sizeof(buf), "#%06X", color); return String(buf);
}
uint32_t hexToColor(String hex) {
  hex.replace("#", ""); return strtoul(hex.c_str(), NULL, 16);
}
// --- THEME SLOTS ---
// Copy a stored slot into the live colour globals (used by the renderer).
void theme_to_globals(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  const GaugeTheme &t = themes[i];
  text_color = t.text; color_low = t.low; color_mid = t.mid; color_high = t.high;
  color_background = t.background; color_mode_label = t.mode_label;
  color_link_icon = t.link_icon; needle_color = t.needle; color_peak = t.peak;
  color_background2 = t.bg_grad2; color_background3 = t.bg_grad3;
  bg_grad_type = t.bg_grad_type; bg_grad_stops = t.bg_grad_stops; bg_grad_angle = t.bg_grad_angle;
}
// Capture the current live colour globals back into a slot (after a web edit).
void globals_to_theme(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  GaugeTheme &t = themes[i];
  t.text = text_color; t.low = color_low; t.mid = color_mid; t.high = color_high;
  t.background = color_background; t.mode_label = color_mode_label;
  t.link_icon = color_link_icon; t.needle = needle_color; t.peak = color_peak;
  t.bg_grad2 = color_background2; t.bg_grad3 = color_background3;
  t.bg_grad_type = bg_grad_type; t.bg_grad_stops = bg_grad_stops; t.bg_grad_angle = bg_grad_angle;
}
// NVS key for one slot field, e.g. "t2_bg" (stays well under the 15-char limit).
static void theme_key(char *buf, uint8_t slot, const char *field) {
  snprintf(buf, 8, "t%u_%s", slot, field);
}
// Persist one slot to NVS. Opens/closes prefs itself — call when prefs are closed.
void persist_theme(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  char k[8]; const GaugeTheme &t = themes[i];
  preferences.begin("gauge", false);
  theme_key(k,i,"tx"); preferences.putUInt(k, t.text);
  theme_key(k,i,"lo"); preferences.putUInt(k, t.low);
  theme_key(k,i,"mi"); preferences.putUInt(k, t.mid);
  theme_key(k,i,"hi"); preferences.putUInt(k, t.high);
  theme_key(k,i,"bg"); preferences.putUInt(k, t.background);
  theme_key(k,i,"ml"); preferences.putUInt(k, t.mode_label);
  theme_key(k,i,"li"); preferences.putUInt(k, t.link_icon);
  theme_key(k,i,"nd"); preferences.putUInt(k, t.needle);
  theme_key(k,i,"pk"); preferences.putUInt(k, t.peak);
  theme_key(k,i,"b2"); preferences.putUInt(k, t.bg_grad2);
  theme_key(k,i,"b3"); preferences.putUInt(k, t.bg_grad3);
  theme_key(k,i,"gt"); preferences.putUChar(k, t.bg_grad_type);
  theme_key(k,i,"gs"); preferences.putUChar(k, t.bg_grad_stops);
  theme_key(k,i,"ga"); preferences.putUShort(k, t.bg_grad_angle);
  preferences.end();
}
// Load all four slots from NVS. Slot 0 defaults to the already-loaded live theme
// (the legacy ct/cl/... keys), so an existing single theme migrates seamlessly;
// slots 1-3 default to the distinct factory palettes. Call with prefs closed.
void load_all_themes() {
  char k[8];
  for (uint8_t i = 0; i < THEME_SLOTS; i++) {
    GaugeTheme d = THEME_DEFAULTS[i];
    if (i == 0) {  // slot 0 default = the current live (legacy) theme
      d.text = text_color; d.low = color_low; d.mid = color_mid; d.high = color_high;
      d.background = color_background; d.mode_label = color_mode_label;
      d.link_icon = color_link_icon; d.needle = needle_color; d.peak = color_peak;
      d.bg_grad2 = color_background2; d.bg_grad3 = color_background3;
      d.bg_grad_type = bg_grad_type; d.bg_grad_stops = bg_grad_stops; d.bg_grad_angle = bg_grad_angle;
    }
    preferences.begin("gauge", true);
    theme_key(k,i,"tx"); themes[i].text       = preferences.getUInt(k, d.text);
    theme_key(k,i,"lo"); themes[i].low        = preferences.getUInt(k, d.low);
    theme_key(k,i,"mi"); themes[i].mid        = preferences.getUInt(k, d.mid);
    theme_key(k,i,"hi"); themes[i].high       = preferences.getUInt(k, d.high);
    theme_key(k,i,"bg"); themes[i].background = preferences.getUInt(k, d.background);
    theme_key(k,i,"ml"); themes[i].mode_label = preferences.getUInt(k, d.mode_label);
    theme_key(k,i,"li"); themes[i].link_icon  = preferences.getUInt(k, d.link_icon);
    theme_key(k,i,"nd"); themes[i].needle     = preferences.getUInt(k, d.needle);
    theme_key(k,i,"pk"); themes[i].peak       = preferences.getUInt(k, d.peak);
    theme_key(k,i,"b2"); themes[i].bg_grad2   = preferences.getUInt(k, d.bg_grad2);
    theme_key(k,i,"b3"); themes[i].bg_grad3   = preferences.getUInt(k, d.bg_grad3);
    theme_key(k,i,"gt"); themes[i].bg_grad_type  = preferences.getUChar(k, d.bg_grad_type);
    theme_key(k,i,"gs"); themes[i].bg_grad_stops = preferences.getUChar(k, d.bg_grad_stops);
    theme_key(k,i,"ga"); themes[i].bg_grad_angle = preferences.getUShort(k, d.bg_grad_angle);
    preferences.end();
  }
}

void handleRoot() {
  String html;
  html.reserve(7400); // pre-allocate to avoid repeated heap reallocs under PSRAM pressure
  html = "<html><head><meta charset='utf-8'><meta name='viewport' content='width=device-width, initial-scale=1'><style>"
         "*{box-sizing:border-box}"
         "body{font-family:system-ui,'Segoe UI',Roboto,sans-serif;background:#0a0b0d;color:#e9eaec;margin:0 auto;padding:16px;max-width:540px}"
         "h1{font-size:21px;letter-spacing:3px;text-transform:uppercase;margin:0;font-weight:700;display:flex;align-items:center}"
         "h1::before{content:'';width:5px;height:22px;background:#ff8a00;margin-right:11px;border-radius:2px;box-shadow:0 0 8px rgba(255,138,0,.6)}"
         ".status{display:flex;gap:8px;flex-wrap:wrap;margin:10px 0}"
         ".chip{background:#1c1f25;border:1px solid #2a2e36;border-radius:20px;padding:4px 12px;font-size:11px;letter-spacing:1px;color:#868d97;text-transform:uppercase}"
         ".chip b{color:#ff8a00;font-weight:700}"
         ".card{background:#14161a;border:1px solid #2a2e36;border-left:3px solid #ff8a00;border-radius:10px;padding:15px;margin:13px 0}"
         ".lbl,.card h3{font-size:11px;letter-spacing:2.5px;text-transform:uppercase;color:#868d97;font-weight:600}"
         ".card h3{margin:0 0 13px}.lbl{margin:16px 4px 6px;display:block}.card h4{font-size:13px;margin:0 0 6px}"
         ".row{display:grid;grid-template-columns:1fr 1fr;gap:9px}"
         "button{font-size:15px;padding:11px 10px;border:1px solid #2a2e36;border-radius:8px;background:#1c1f25;color:#e9eaec;cursor:pointer;transition:.13s;width:100%;font-family:inherit;letter-spacing:.5px}"
         "button:hover{border-color:#ff8a00;background:#23262d}button:active{transform:translateY(1px)}"
         ".on,.on:hover{background:rgba(30,215,96,.15);border-color:#1ed760;color:#1ed760;box-shadow:0 0 12px rgba(30,215,96,.22)}"
         ".active,.active:hover{background:rgba(255,138,0,.16);border-color:#ff8a00;color:#ff8a00;box-shadow:0 0 12px rgba(255,138,0,.25)}"
         ".primary,.primary:hover{background:#ff8a00;border-color:#ff8a00;color:#0a0b0d;font-weight:700}"
         ".danger,.danger:hover{border-color:#ff3b30;color:#ff3b30}"
         "label{color:#868d97;font-size:14px}.crow{display:flex;justify-content:space-between;align-items:center;margin:8px 0}"
         "input[type=range]{-webkit-appearance:none;appearance:none;width:100%;height:6px;border-radius:3px;background:#2a2e36;outline:none;margin:4px 0 10px}"
         "input[type=range]::-webkit-slider-thumb{-webkit-appearance:none;width:20px;height:20px;border-radius:50%;background:#ff8a00;cursor:pointer;box-shadow:0 0 8px rgba(255,138,0,.7)}"
         "input[type=range]::-moz-range-thumb{width:20px;height:20px;border:none;border-radius:50%;background:#ff8a00}"
         "input[type=color]{width:46px;height:34px;border:1px solid #2a2e36;border-radius:6px;background:none;padding:2px;cursor:pointer}"
         "input[type=text]{background:#1c1f25;color:#e9eaec;border:1px solid #2a2e36;border-radius:8px;padding:10px;font-size:15px;width:100%;margin-bottom:9px}"
         "select{background:#1c1f25;color:#e9eaec;border:1px solid #2a2e36;border-radius:8px;padding:9px;font-size:14px}"
         "small{color:#5f6670;font-size:12px}#brval{color:#ff8a00;font-weight:700;font-size:18px}"
         "</style></head><body>";
  html += "<h1>" + device_name + "</h1>";
  html += "<div class='status'><span class='chip'><b id='chippeers'>" + String(fleet_count) + "</b> peers</span><span class='chip'>Mode <b id='chipmode'>" + String(MODE_NAMES[current_mode]) + "</b></span><span class='chip'>Page <b id='chippage'>" + String(current_page==PAGE_GAUGE?"GAUGE":"GLOWCRAFT") + "</b></span><span class='chip'>Theme <b id='chipslot'>P" + String(active_theme) + "</b>" + String(trimpot_theme_sync?" \xE2\x9F\xB2":"") + "</span></div>";
  html += "<div class='card'><h3>Device Name</h3><form action='/name' method='get'><input type='text' name='n' value='" + device_name + "' maxlength='20'><button class='danger'>Rename</button></form><small>WiFi AP name (Haltech-[name]). Restarts device.</small></div>";
  
  html += "<div class='card'><h3>Trimpot Theme Sync</h3>";
  html += "<button id='tpsync' class='" + String(trimpot_theme_sync?"on":"") + "' onclick=\"tgl('/themesync','tpsync','Trim Sync: ')\">Trim Sync: " + String(trimpot_theme_sync?"ON":"OFF") + "</button>";
  html += "<span class='lbl'>Editing slot (Rotary Trim 3 position)</span><div class='row'>";
  for (int i = 0; i < THEME_SLOTS; i++)
    html += "<button id='sl" + String(i) + "' class='sl" + String(active_theme==i?" active":"") + "' onclick=\"setSlot(" + String(i) + ",this)\">P" + String(i) + "</button>";
  html += "</div><small>The colours below edit the selected slot. With Trim Sync ON, Rotary Trim 3 selects the live slot automatically.</small></div>";

  html += "<div class='card'><h3>Dynamic Elements</h3><form action='/theme' method='get' onsubmit='return subm(event,this)'><div class='crow'><label>Text</label><input type='color' name='ct' value='" + colorToHex(text_color) + "'></div><div class='crow'><label>Low</label><input type='color' name='cl' value='" + colorToHex(color_low) + "'></div><div class='crow'><label>Mid</label><input type='color' name='cm' value='" + colorToHex(color_mid) + "'></div><div class='crow'><label>High</label><input type='color' name='ch' value='" + colorToHex(color_high) + "'></div><button class='primary'>Apply to ALL</button></form></div>";



  html += "<div class='card'><h3>Static Elements</h3><form action='/uicolors' method='get' onsubmit='return subm(event,this)'><div class='crow'><label>Background</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div><div class='crow'><label>Mode Label</label><input type='color' name='cml' value='" + colorToHex(color_mode_label) + "'></div><div class='crow'><label>Link Icon</label><input type='color' name='cli' value='" + colorToHex(color_link_icon) + "'></div><div class='crow'><label>Needle</label><input type='color' name='cn' value='" + colorToHex(needle_color) + "'></div><div class='crow'><label>Peak Stripe</label><input type='color' name='cp' value='" + colorToHex(color_peak) + "'></div><button class='primary'>Apply to ALL</button></form></div>";

  // BACKGROUND GRADIENT (per active slot). Stop 1 = the slot background colour.
  {
    const char* GT[6] = {"Solid","Linear \xE2\x86\x95","Linear \xE2\x86\x94","Linear \xE2\x88\xA0","Radial","Conical"};
    html += "<div class='card'><h3>Background Gradient</h3><form action='/grad' method='get' onsubmit='return subm(event,this)'>";
    html += "<div class='crow'><label>Type</label><select name='gt' id='gt' onchange='gradUI()'>";
    for (int i = 0; i < 6; i++) html += "<option value='" + String(i) + "'" + String(bg_grad_type==i?" selected":"") + ">" + String(GT[i]) + "</option>";
    html += "</select></div>";
    html += "<div class='crow'><label>Stops</label><select name='gs' id='gs' onchange='gradUI()'><option value='2'" + String(bg_grad_stops==2?" selected":"") + ">2</option><option value='3'" + String(bg_grad_stops==3?" selected":"") + ">3</option></select></div>";
    html += "<div class='crow'><label>Stop 1 (Background)</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div>";
    html += "<div class='crow'><label>Stop 2</label><input type='color' name='b2' value='" + colorToHex(color_background2) + "'></div>";
    html += "<div class='crow' id='b3row'><label>Stop 3</label><input type='color' name='b3' value='" + colorToHex(color_background3) + "'></div>";
    html += "<div class='crow' id='garow'><label>Angle</label><span id='gaval'>" + String(bg_grad_angle) + "\xC2\xB0</span></div>";
    html += "<input type='range' id='ga' name='ga' min='0' max='360' value='" + String(bg_grad_angle) + "' oninput=\"document.getElementById('gaval').textContent=this.value+'\xC2\xB0'\">";
    html += "<button class='primary'>Apply to ALL</button></form><small>Stop 1 is the slot background. Angle applies to Linear \xE2\x88\xA0 and Conical.</small></div>";
  }

  html += "<div class='card'><h3>Global Controls</h3>";
  html += "<div class='crow'><label>Brightness</label><span id='brval'>" + String(current_brightness) + "</span></div>";
  html += "<input type='range' min='10' max='100' value='" + String(current_brightness) + "' oninput=\"document.getElementById('brval').textContent=this.value\" onchange=\"setBright(this.value,this)\">";
  html += "<div class='row'>";
  html += "<button id='test' class='" + String(test_mode_enabled?"on":"") + "' onclick=\"tgl('/test','test','Test: ')\">Test: " + String(test_mode_enabled?"ON":"OFF") + "</button>";
  html += "<button id='stats' class='" + String(show_perf_stats?"on":"") + "' onclick=\"tgl('/stats','stats','Stats: ')\">Stats: " + String(show_perf_stats?"ON":"OFF") + "</button>";
  html += "<button id='dbg' class='" + String(debug_mode_enabled?"on":"") + "' onclick=\"tgl('/debug','dbg','Debug: ')\">Debug: " + String(debug_mode_enabled?"ON":"OFF") + "</button>";
  html += "<button id='font' onclick=\"tglFont(this)\">Font: " + String(current_font == 0 ? "DSEG14" : "Fira Mono") + "</button>";
  html += "</div><div class='row'>";
  html += "<button onclick=\"location='/preview'\">Live Preview</button>";
  html += "<button onclick=\"location='/ota'\">OTA Update</button>";
  html += "</div></div>";

  html += "<div class='card'><h3>Display Page</h3><div class='row'>";
  html += "<button id='pg0' class='pg" + String(current_page==PAGE_GAUGE?" active":"") + "' onclick=\"setPage(0,this)\">Gauge</button>";
  html += "<button id='pg1' class='pg" + String(current_page==PAGE_GLOWCRAFT?" active":"") + "' onclick=\"setPage(1,this)\">GlowCraft</button>";
  html += "</div></div>";

  html += "<div class='card'><h3>Local Gauge</h3>";
  html += "<button id='peak' class='" + String(peak_hold_enabled?"on":"") + "' onclick=\"tgl('/peak','peak','Peak Hold: ')\">Peak Hold: " + String(peak_hold_enabled?"ON":"OFF") + "</button>";

  // SECONDARY METRIC (shown when peak hold is off)
  html += "<span class='lbl'>Secondary metric (Peak Hold OFF)</span>";
  html += "<form action='/secondary' method='get' onsubmit='return subm(event,this)' style='display:flex;gap:8px;align-items:center'>";
  html += "<select name='sm' style='flex:1'>";
  for (int i = 0; i < SECONDARY_COUNT; i++) {
    html += "<option value='" + String(i) + "'";
    if (secondary_metric == i) html += " selected";
    html += ">" + String(SECONDARY_NAMES[i]) + "</option>";
  }
  html += "</select>";
  html += "<button type='submit' class='primary' style='width:auto;padding:9px 16px'>Set</button>";
  html += "</form>";

  html += "<span class='lbl'>Display Metric</span><div class='row'>";
  html += "<button class='m" + String(current_mode==0?" active":"") + "' onclick=\"setMode(0,this)\">Boost</button>";
  html += "<button class='m" + String(current_mode==1?" active":"") + "' onclick=\"setMode(1,this)\">AFR</button>";
  html += "<button class='m" + String(current_mode==2?" active":"") + "' onclick=\"setMode(2,this)\">Water</button>";
  html += "<button class='m" + String(current_mode==3?" active":"") + "' onclick=\"setMode(3,this)\">Oil</button>";
  html += "</div></div>";
  
  {
    PeerGauge peers[10];                       // consistent copy — fleet[] is written from the ESP-NOW callback
    int n_peers = fleet_snapshot(peers, 10);
    if (n_peers > 0) {
      html += "<span class='lbl'>Remote Gauges</span>";
      for(int i=0; i<n_peers; i++) {
          if (millis() - peers[i].last_seen < 10000) {
              String macStr = "";
              for(int j=0; j<6; j++) { if(j>0) macStr += ":"; char buf[3]; sprintf(buf, "%02X", peers[i].mac[j]); macStr += buf; }
              String macClean = macStr; macClean.replace(":", "");
              html += "<div class='card'><h4>Gauge " + macClean.substring(9) + "</h4><div class='status'><span class='chip'>Mode <b>" + String(MODE_NAMES[constrain(peers[i].mode,0,3)]) + "</b></span></div><div class='row'><button onclick=\"rem('/rem?mac=" + macClean + "&mode=0',this)\">Boost</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=1',this)\">AFR</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=2',this)\">Water</button><button onclick=\"rem('/rem?mac=" + macClean + "&mode=3',this)\">Oil</button></div></div>";
          }
      }
    }
  }
  // Background-fetch helpers. Each control updates from the server's reported new
  // state and flashes green on confirmed success / red if the request failed.
  html += "<script>"
          "function flash(b,ok){if(!b)return;b.style.boxShadow='0 0 16px '+(ok?'#1ed760':'#ff3b30');setTimeout(function(){b.style.boxShadow='';},450);}"
          "function gt(u){return fetch(u).then(function(r){if(!r.ok)throw 0;return r.text();});}"
          "function tgl(u,id,pre){var b=document.getElementById(id);gt(u).then(function(s){var on=s.trim()=='1';b.textContent=pre+(on?'ON':'OFF');b.classList.toggle('on',on);flash(b,1);}).catch(function(){flash(b,0);});}"
          "var MN=['BOOST','AFR','WATER','OIL P'];"
          "function setMode(m,b){gt('/set?mode='+m).then(function(s){var i=parseInt(s),c=document.getElementById('chipmode');if(c)c.textContent=MN[i];document.querySelectorAll('.m').forEach(function(x){x.classList.remove('active')});b.classList.add('active');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function tglFont(b){gt('/font').then(function(s){b.textContent='Font: '+(parseInt(s)==0?'DSEG14':'Fira Mono');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function setPage(p,b){gt('/page?pg='+p).then(function(s){var pg=parseInt(s),c=document.getElementById('chippage');if(c)c.textContent=pg==0?'GAUGE':'GLOWCRAFT';document.querySelectorAll('.pg').forEach(function(x){x.classList.remove('active')});b.classList.add('active');flash(b,1);}).catch(function(){flash(b,0);});}"
          "function post(u,b){return gt(u).then(function(s){flash(b,true);return s;}).catch(function(){flash(b,false);});}"
          "function subm(ev,f){ev.preventDefault();post(f.getAttribute('action')+'?'+new URLSearchParams(new FormData(f)).toString(),f.querySelector('button'));return false;}"
          "function setBright(v,b){post('/bright?b='+v,b);}"
          "function rem(u,b){post(u,b);}"
          "function setSlot(s,b){gt('/themeslot?s='+s).then(function(){location.reload();}).catch(function(){flash(b,0);});}"
          // Show/hide gradient sub-controls: stop-3 only when 3 stops, angle only for Linear-angle(3)/Conical(5).
          "function gradUI(){var t=+document.getElementById('gt').value,s=+document.getElementById('gs').value;"
          "document.getElementById('b3row').style.display=(t!=0&&s==3)?'':'none';"
          "var ang=(t==3||t==5);document.getElementById('garow').style.display=ang?'':'none';document.getElementById('ga').style.display=ang?'':'none';}"
          "gradUI();"
          "</script>";
  html += "<footer style='text-align:center;opacity:0.5;font-size:12px;margin:24px 0 10px'>v" FIRMWARE_VERSION " &middot; built " FIRMWARE_BUILD "</footer>";
  html += "</body></html>";
  server.send(200, "text/html", html);
}

void handleTheme() {
    if (server.hasArg("ct")) {
        text_color = hexToColor(server.arg("ct"));
        color_low = hexToColor(server.arg("cl"));
        color_mid = hexToColor(server.arg("cm"));
        color_high = hexToColor(server.arg("ch"));
        preferences.begin("gauge", false); preferences.putUInt("ct", text_color); preferences.putUInt("cl", color_low); preferences.putUInt("cm", color_mid); preferences.putUInt("ch", color_high); preferences.end();
        EspNowPacket pkt = {}; pkt.type = 3; pkt.c1=text_color; pkt.c2=color_low; pkt.c3=color_mid; pkt.c4=color_high;
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}
void handleSet() {
    if (server.hasArg("mode")) {
        int m = constrain(server.arg("mode").toInt(), 0, 3);
        Serial.printf("[HTTP] handleSet: local mode -> %d (live)\n", m);
        current_mode = (GaugeMode)m;
        preferences.begin("gauge", false); preferences.putInt("mode", m); preferences.end();
        flag_mode_update = true;   // apply live — no restart
    }
    // Reply with the applied mode index — the button confirms from this response.
    server.send(200, "text/plain", String((int)current_mode));
}
// Toggle handlers flip server-side and return the new state ("1"/"0") so the web
// UI can confirm the change actually took, rather than guessing optimistically.
void handleTest() {
    test_mode_enabled = !test_mode_enabled;
    EspNowPacket pkt = {}; pkt.type = 4; pkt.value = test_mode_enabled?1:0; broadcast_packet(&pkt);
    server.send(200, "text/plain", test_mode_enabled ? "1" : "0");
}
void handleStats() {
    show_perf_stats = !show_perf_stats;
    EspNowPacket pkt = {}; pkt.type = 6; pkt.value = show_perf_stats?1:0; broadcast_packet(&pkt);
    flag_stats_update = true;
    server.send(200, "text/plain", show_perf_stats ? "1" : "0");
}
void handleDebug() {
    debug_mode_enabled = !debug_mode_enabled;
    preferences.begin("gauge", false);
    preferences.putBool("dbg", debug_mode_enabled);
    preferences.end();
    server.send(200, "text/plain", debug_mode_enabled ? "1" : "0");
}
void handleBright() {
    if (server.hasArg("b")) {
        int b = constrain(server.arg("b").toInt(), 10, 100);
        current_brightness = b; set_backlight(b);
        preferences.begin("gauge", false); preferences.putInt("bright", b); preferences.end();
        EspNowPacket pkt = {}; pkt.type = 5; pkt.value = b; broadcast_packet(&pkt);
    }
    server.send(200, "text/plain", String(current_brightness));
}
void handlePeak() {
    peak_hold_enabled = !peak_hold_enabled;
    if (peak_hold_enabled) { peak_val = -999.0f; peak_low_val = 999.0f; }
    preferences.begin("gauge", false); preferences.putBool("peak", peak_hold_enabled); preferences.end();
    server.send(200, "text/plain", peak_hold_enabled ? "1" : "0");
}
void handleSecondary() {
    if (server.hasArg("sm"))
        secondary_metric = (uint8_t)constrain(server.arg("sm").toInt(), 0, SECONDARY_COUNT - 1);
    preferences.begin("gauge", false);
    preferences.putUInt("sm", secondary_metric);
    preferences.end();
    server.send(200, "text/plain", String((int)secondary_metric));
}
void handlePage() {
    if (server.hasArg("pg")) {
        int p = constrain(server.arg("pg").toInt(), 0, 1);
        current_page = (DisplayPage)p;
        preferences.begin("gauge", false);
        preferences.putUInt("page", (uint32_t)current_page);
        preferences.end();
        flag_page_update = true;
    }
    server.send(200, "text/plain", String((int)current_page));
}
void handleFont() {
    current_font = (current_font == 0) ? 1 : 0;   // two fonts: DSEG14 (0) / Fira Mono (1)
    preferences.begin("gauge", false);
    preferences.putUInt("font", current_font);
    preferences.end();
    flag_theme_update = true;
    server.send(200, "text/plain", String((int)current_font));
}
// Capture the live RGB framebuffer and serve it as a BMP image.
// The RGB panel keeps 2 PSRAM framebuffers; we grab whichever is current.
// Row-streamed: converts + sends one row at a time from a static buffer, so a
// snapshot no longer allocates ~691KB of PSRAM per request. Rate-limited since
// each request still blocks the loop (LVGL) for the duration of the send.
// The framebuffer is read unlocked while the panel scans it — occasional
// tearing is accepted for a diagnostic view; do NOT lock against the vsync path.
void handleSnapshot() {
    static uint32_t last_snap_ms = 0;
    if (last_snap_ms && millis() - last_snap_ms < 500) {
        server.send(429, "text/plain", "Too fast");
        return;
    }
    last_snap_ms = millis();

    void *fb0 = nullptr, *fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK || fb0 == nullptr) {
        server.send(500, "text/plain", "Framebuffer unavailable");
        return;
    }

    const int W = 480, H = 480;
    const int row_stride = (W * 3 + 3) & ~3;  // BMP rows padded to 4 bytes
    const int file_size  = 54 + row_stride * H;
    static uint8_t rowbuf[(480 * 3 + 3) & ~3];  // one padded BMP row (1440B)

    // --- BMP file (14) + DIB (40) header ---
    uint8_t hdr[54];
    memset(hdr, 0, sizeof(hdr));
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t*)(hdr + 2)  = file_size;
    *(uint32_t*)(hdr + 10) = 54;          // pixel data offset
    *(uint32_t*)(hdr + 14) = 40;          // DIB header size
    *(int32_t*) (hdr + 18) = W;
    *(int32_t*) (hdr + 22) = -H;          // negative = top-down row order
    *(uint16_t*)(hdr + 26) = 1;           // colour planes
    *(uint16_t*)(hdr + 28) = 24;          // bits per pixel (RGB888)
    *(uint32_t*)(hdr + 34) = row_stride * H;

    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    client.write(hdr, sizeof(hdr));

    // --- Convert RGB565 → RGB888 one row at a time and stream it out ---
    const uint16_t* src = (const uint16_t*)fb0;
    memset(rowbuf, 0, sizeof(rowbuf));    // zero the padding bytes once
    for (int y = 0; y < H; y++) {
        const uint16_t* srow = src + y * W;
        for (int x = 0; x < W; x++) {
            uint16_t px = srow[x];
            rowbuf[x*3 + 0] = (px & 0x1F)         << 3;  // B
            rowbuf[x*3 + 1] = ((px >> 5)  & 0x3F) << 2;  // G
            rowbuf[x*3 + 2] = ((px >> 11) & 0x1F) << 3;  // R
        }
        if (client.write(rowbuf, row_stride) != (size_t)row_stride) return;  // client gone
        if ((y & 31) == 31) delay(0);     // feed watchdog / WiFi stack every 32 rows
    }
}

// Auto-refreshing preview page — connect to gauge WiFi, open in browser
void handlePreview() {
    String html = "<!DOCTYPE html><html><head>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>" + device_name + " Preview</title>"
        "<style>body{background:#111;text-align:center;margin:0;padding:10px;font-family:sans-serif;color:#fff}"
        "img{width:480px;height:480px;max-width:100%;border-radius:50%;box-shadow:0 0 30px #333}"
        ".controls{margin:10px} a{color:#aaa;margin:0 10px;text-decoration:none}"
        "</style></head><body>"
        "<h2>" + device_name + "</h2>"
        "<img id='scr' src='/snapshot'>"
        "<div class='controls'>"
        "<a href='/preview'>Refresh</a>"
        "<a href='/'>Config</a>"
        "</div>"
        "<script>"
        "function reload(){document.getElementById('scr').src='/snapshot?t='+Date.now()}"
        "var iv=setInterval(reload,3000);"  // auto-refresh every 3s (snapshot is rate-limited)
        "</script>"
        "</body></html>";
    server.send(200, "text/html", html);
}

void handleOTAPage() {
    String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;text-align:center;background:#222;color:#fff;padding:20px;}";
    html += "input,button{font-size:16px;padding:10px;margin:10px;border-radius:5px;border:none;}";
    html += "button{background:#e65100;color:white;width:200px;cursor:pointer;}</style></head><body>";
    html += "<h2>" + device_name + " - Firmware Update</h2>";
    html += "<p style='color:#aaa'>Select a .bin file built for this device.</p>";
    html += "<form method='POST' action='/ota' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'><br>";
    html += "<button type='submit'>Flash Firmware</button>";
    html += "</form><p><a href='/' style='color:#aaa'>Back</a></p></body></html>";
    server.send(200, "text/html", html);
}

void handleOTAUpload() {
    HTTPUpload& upload = server.upload();
    if (upload.status == UPLOAD_FILE_START) {
        Serial.printf("[OTA] Start: %s\n", upload.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_WRITE) {
        if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
            Update.printError(Serial);
        }
    } else if (upload.status == UPLOAD_FILE_END) {
        if (Update.end(true)) {
            Serial.printf("[OTA] Success: %u bytes written\n", upload.totalSize);
        } else {
            Update.printError(Serial);
        }
    }
}

void handleOTADone() {
    bool ok = !Update.hasError();
    String html = "<html><body style='background:#222;color:#fff;text-align:center;font-family:sans-serif;padding:20px'>";
    if (ok) {
        html += "<h2>Update Successful!</h2><p>Rebooting in 3 seconds...</p>";
        html += "<script>setTimeout(()=>location.href='/',5000)</script>";
    } else {
        html += "<h2>Update Failed</h2><p><a href='/ota' style='color:#aaa'>Try again</a></p>";
    }
    html += "</body></html>";
    server.sendHeader("Connection", "close");
    server.send(200, "text/html", html);
    if (ok) reboot_at_ms = millis() + 800;  // reboot from loop() after the response flushes
}

void handleName() {
    if (server.hasArg("n")) {
        String n = server.arg("n");
        n.trim();
        if (n.length() > 0 && n.length() <= 20) {
            device_name = n;
            preferences.begin("gauge", false);
            preferences.putString("devname", device_name);
            preferences.end();
            server.sendHeader("Location", "/");
            server.send(303);
            reboot_at_ms = millis() + 400;  // restart (from loop) to apply new AP SSID
        } else {
            server.send(400, "text/plain", "Name must be 1-20 characters");
        }
    } else {
        server.send(400, "text/plain", "Missing name");  // was: no response at all (client hung)
    }
}

void handleRemote() {
    if (server.hasArg("mac") && server.hasArg("mode")) {
      String macStr = server.arg("mac");
        int m = server.arg("mode").toInt();
        Serial.printf("[HTTP] handleRemote: remote mode -> %d, mac=%s\n", m, macStr.c_str());
        uint8_t targetMac[6];
        for (int i = 0; i < 6; i++) { String byteStr = macStr.substring(i*2, i*2+2); targetMac[i] = (uint8_t) strtol(byteStr.c_str(), NULL, 16); }
        send_remote_command(targetMac, m);
        server.send(200, "text/plain", "OK");
    } else { server.send(400, "text/plain", "Bad Request"); }
}

void handleUIColors() {
    if (server.hasArg("cbg")) {
        color_background = hexToColor(server.arg("cbg"));
        color_mode_label = hexToColor(server.arg("cml"));
        color_link_icon = hexToColor(server.arg("cli"));
        needle_color = hexToColor(server.arg("cn"));
        color_peak = hexToColor(server.arg("cp"));
        preferences.begin("gauge", false);
        preferences.putUInt("cbg", color_background);
        preferences.putUInt("cml", color_mode_label);
        preferences.putUInt("cli", color_link_icon);
        preferences.putUInt("cn", needle_color);
        preferences.putUInt("cp", color_peak);
        preferences.end();
        // Broadcast UI colors to fleet
        EspNowPacket pkt = {}; 
        pkt.type = 7;
        pkt.c1 = color_background;
        pkt.c2 = color_mode_label;
        pkt.c3 = color_link_icon;
        pkt.c4 = needle_color;
        pkt.value = (int)color_peak;
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}

// Background gradient editor. Sets the live gradient globals, persists the legacy
// live keys (for slot-0 migration), broadcasts to the fleet (type 8), and folds the
// edit into the active theme slot — mirroring handleUIColors().
void handleGrad() {
    if (server.hasArg("gt")) {
        color_background  = hexToColor(server.arg("cbg"));
        color_background2 = hexToColor(server.arg("b2"));
        color_background3 = hexToColor(server.arg("b3"));
        bg_grad_type  = (uint8_t)constrain(server.arg("gt").toInt(), 0, 5);
        bg_grad_stops = (server.arg("gs").toInt() == 3) ? 3 : 2;
        bg_grad_angle = (uint16_t)constrain(server.arg("ga").toInt(), 0, 360);
        preferences.begin("gauge", false);
        preferences.putUInt("cbg", color_background);
        preferences.putUInt("cbg2", color_background2);
        preferences.putUInt("cbg3", color_background3);
        preferences.putUChar("cgt", bg_grad_type);
        preferences.putUChar("cgs", bg_grad_stops);
        preferences.putUShort("cga", bg_grad_angle);
        preferences.end();
        // Fleet sync: c1/c2 = stops 2/3, c3 = background (stop 1), value packs type|stops|angle.
        EspNowPacket pkt = {}; pkt.type = 8;
        pkt.c1 = color_background2; pkt.c2 = color_background3; pkt.c3 = color_background;
        pkt.value = (int)((uint32_t)bg_grad_type | ((uint32_t)bg_grad_stops << 4) | ((uint32_t)bg_grad_angle << 8));
        broadcast_packet(&pkt);
        flag_theme_update = true;
        globals_to_theme(active_theme); persist_theme(active_theme);  // edit lands in the active slot
    }
    server.send(200, "text/plain", "OK");
}

// Toggle the rotary-driven theme link. Enabling it snaps to the rotary's last
// known position immediately so the user doesn't have to twist the knob first.
void handleThemeSync() {
    trimpot_theme_sync = !trimpot_theme_sync;
    preferences.begin("gauge", false); preferences.putBool("tpsync", trimpot_theme_sync); preferences.end();
    if (trimpot_theme_sync && last_trimpot3 >= 0 && last_trimpot3 < THEME_SLOTS) {
        active_theme = (uint8_t)last_trimpot3;
        preferences.begin("gauge", false); preferences.putUInt("atheme", active_theme); preferences.end();
        theme_to_globals(active_theme);
        flag_theme_update = true;
    }
    server.send(200, "text/plain", trimpot_theme_sync ? "1" : "0");
}

// Select which slot is live / being edited (web-side preview; rotary overrides
// this when sync is on and the knob moves).
void handleThemeSlot() {
    if (server.hasArg("s")) {
        active_theme = (uint8_t)constrain(server.arg("s").toInt(), 0, THEME_SLOTS - 1);
        preferences.begin("gauge", false); preferences.putUInt("atheme", active_theme); preferences.end();
        theme_to_globals(active_theme);
        flag_theme_update = true;
    }
    server.send(200, "text/plain", String((int)active_theme));
}

void setup_wifi() {
  WiFi.mode(WIFI_AP_STA);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Haltech-%s", device_name.c_str());
  if (!WiFi.softAP(ssid, NULL, WIFI_CHANNEL)) {
    Serial.println("[WIFI] softAP FAILED (out of internal heap?) — web/OTA unreachable");
  }

  // Reduce WiFi power to minimize RF interference with display PSRAM bus
  esp_wifi_set_max_tx_power(34); // Reduce to ~8.5dBm to minimise PSRAM bus contention during TX bursts

  // Cache our own MACs once — OnDataRecv filters self-echoes against these
  // instead of calling into the WiFi driver per packet.
  WiFi.macAddress(my_sta_mac);
  WiFi.softAPmacAddress(my_ap_mac);

  // Web server + OTA are the recovery path — bring them up BEFORE ESP-NOW so a
  // fleet-sync init failure can never take down the ability to re-flash.
  // (Previously `if (esp_now_init() != ESP_OK) return;` skipped server.begin().)
  server.on("/", handleRoot);
  server.on("/theme", handleTheme); server.on("/set", handleSet); server.on("/rem", handleRemote); server.on("/name", handleName);
  server.on("/bright", handleBright); server.on("/test", handleTest); server.on("/stats", handleStats); server.on("/debug", handleDebug);
  server.on("/peak", handlePeak); server.on("/uicolors", handleUIColors); server.on("/font", handleFont);
  server.on("/secondary", handleSecondary); server.on("/page", handlePage);
  server.on("/themesync", handleThemeSync); server.on("/themeslot", handleThemeSlot);
  server.on("/grad", handleGrad);
  server.on("/preview", handlePreview); server.on("/snapshot", handleSnapshot);
  server.on("/ota", HTTP_GET, handleOTAPage);
  server.on("/ota", HTTP_POST, handleOTADone, handleOTAUpload);
  server.begin();

  // ArduinoOTA — allows PlatformIO upload direct over WiFi
  // Connect PC to gauge AP, then: pio run -t upload --upload-port 192.168.4.1
  ArduinoOTA.setHostname(device_name.c_str());
  ArduinoOTA.onStart([]()  { Serial.println("[OTA] ArduinoOTA start"); });
  ArduinoOTA.onEnd([]()    { Serial.println("[OTA] ArduinoOTA done"); });
  ArduinoOTA.onError([](ota_error_t e) { Serial.printf("[OTA] Error %u\n", e); });
  ArduinoOTA.begin();

  // ESP-NOW last — fleet sync is optional; web/OTA above must survive its failure.
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
  } else {
    Serial.println("[ESPNOW] init FAILED — fleet sync disabled this boot");
  }
}

// --- UI ---
void common_label_setup() {
  val_label_int = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_int, true, 0);

  val_label_dec = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_dec, true, 0);

    mode_label = lv_label_create(gauge_scr);
    #ifdef LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_28, 0);
    #else
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_set_style_text_color(mode_label, lv_color_hex(color_mode_label), 0);
    lv_label_set_text(mode_label, MODE_NAMES[current_mode]);
#if FONT_FIRAMONO_AVAILABLE
    const lv_font_t* font_large = (current_font == 1) ? &firamono_120 : &dseg14_120;
    const lv_font_t* font_mid   = (current_font == 1) ? &firamono_96  : &dseg14_96;
#else
    const lv_font_t* font_large = &dseg14_120;
    const lv_font_t* font_mid   = &dseg14_96;
#endif
    lv_obj_set_style_text_font(val_label_int, font_large, 0);
    lv_obj_set_style_text_font(val_label_dec, font_mid, 0);
}

// Paint the gauge-screen background: a flat colour when bg_grad_type==0, otherwise a
// 2/3-stop gradient (vertical, horizontal, angled-linear, radial, or conical). The
// descriptor must persist — the style stores a pointer to it, not a copy — so it is
// static. Screen is the fixed 480x480 round panel; centre is (240,240).
static lv_grad_dsc_t bg_grad_dsc;
void apply_background(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(color_background), 0);  // stop 1 / solid fallback
    if (bg_grad_type == 0) {
        lv_obj_set_style_bg_grad(scr, NULL, 0);  // clear any gradient left by a previous slot
        return;
    }
    uint8_t n = (bg_grad_stops == 3) ? 3 : 2;
    lv_color_t cols[3] = { lv_color_hex(color_background),
                           lv_color_hex(color_background2),
                           lv_color_hex(color_background3) };
    lv_grad_init_stops(&bg_grad_dsc, cols, NULL, NULL, n);  // NULL fracs/opa = even stops, opaque

    const int32_t W = 480, H = 480, CX = 240, CY = 240;
    switch (bg_grad_type) {
        case 1:  // linear vertical (top -> bottom)
            lv_grad_linear_init(&bg_grad_dsc, 0, 0, 0, H, LV_GRAD_EXTEND_PAD); break;
        case 2:  // linear horizontal (left -> right)
            lv_grad_linear_init(&bg_grad_dsc, 0, 0, W, 0, LV_GRAD_EXTEND_PAD); break;
        case 3: { // linear at an angle, through the centre
            float a = bg_grad_angle * 3.14159265f / 180.0f;
            int32_t dx = (int32_t)(cosf(a) * 340.0f), dy = (int32_t)(sinf(a) * 340.0f);
            lv_grad_linear_init(&bg_grad_dsc, CX - dx, CY - dy, CX + dx, CY + dy, LV_GRAD_EXTEND_PAD);
            break;
        }
        case 4:  // radial: centre -> right edge (radius 240)
            lv_grad_radial_init(&bg_grad_dsc, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT, LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD); break;
        case 5: {  // conical sweep starting at bg_grad_angle (keep angles in 0..360)
            int16_t a0 = (int16_t)(bg_grad_angle % 360);
            lv_grad_conical_init(&bg_grad_dsc, LV_GRAD_CENTER, LV_GRAD_CENTER, a0, a0 + 359, LV_GRAD_EXTEND_PAD);
            break;
        }
        default:
            lv_obj_set_style_bg_grad(scr, NULL, 0); return;
    }
    lv_obj_set_style_bg_grad(scr, &bg_grad_dsc, 0);
}

void load_current_style() {
    lv_obj_clean(gauge_scr);
    apply_background(gauge_scr);

    // LINK ICON
    link_icon = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(link_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(link_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(link_icon, lv_color_hex(color_link_icon), 0);
    lv_obj_align(link_icon, LV_ALIGN_BOTTOM_MID, 0, -100); 
    if(fleet_count == 0) lv_obj_add_flag(link_icon, LV_OBJ_FLAG_HIDDEN);

    // PERF OVERLAY (MOVED TO CENTER-TOP)
    perf_label = lv_label_create(gauge_scr);
    lv_obj_align(perf_label, LV_ALIGN_CENTER, 0, -140); // Move performance monitor above main text
    lv_obj_set_style_text_color(perf_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(perf_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(perf_label, 150, 0); 
    if(!show_perf_stats) lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);

    // === STATIC RING INDICATOR (outer border, color-changing) ===
    bar = lv_obj_create(gauge_scr);
    const int ring_size = 480; // square (w == h)
    lv_obj_set_size(bar, ring_size, ring_size);  // Fill most of screen
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 16, 0);  // Thick border for ring
    lv_obj_set_style_border_color(bar, lv_color_hex(color_low), 0);  // Start with low color
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(bar, ring_size / 2, 0);  // Full circle (half of size)

    peak_dot = lv_obj_create(gauge_scr);
    lv_obj_set_size(peak_dot, 8, 8);  // Small indicator dot (hidden for now)
    lv_obj_set_style_radius(peak_dot, 4, 0);
    lv_obj_set_style_bg_color(peak_dot, lv_color_hex(color_peak), 0);
    lv_obj_set_style_border_width(peak_dot, 0, 0);
    lv_obj_set_pos(peak_dot, 0, 0);
    if(!peak_hold_enabled) lv_obj_add_flag(peak_dot, LV_OBJ_FLAG_HIDDEN); // Initial State
    
    // NEEDLE TIP - thin triangle that orbits around center
    needle_tip = lv_line_create(gauge_scr);
    lv_obj_set_style_line_width(needle_tip, 8, 0);  // Thin line width
    lv_obj_set_style_line_color(needle_tip, lv_color_hex(needle_color), 0);
    lv_obj_set_style_line_rounded(needle_tip, 0, 0);  // Not rounded
    
    // PEAK HIGH / LOW LABELS (top section, same font as mode label)
    peak_high_label = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(peak_high_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(peak_high_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_align(peak_high_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(peak_high_label, LV_SYMBOL_UP " --");
    lv_obj_align(peak_high_label, LV_ALIGN_CENTER, -60, -120);
    if (!peak_hold_enabled) lv_obj_add_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);

    peak_low_label = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(peak_low_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(peak_low_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_align(peak_low_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(peak_low_label, LV_SYMBOL_DOWN " --");
    lv_obj_align(peak_low_label, LV_ALIGN_CENTER, 60, -120);
    if (!peak_hold_enabled) lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);

    common_label_setup();
    lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_align(val_label_int, LV_ALIGN_CENTER, 50, -10); // Move right 40 px, up 10 px
    lv_obj_align(val_label_dec, LV_ALIGN_CENTER, 50, -10);  // Move right 40 px, up 10 px

    current_applied_text = 0;
}

// Apply the current theme's colours (plus background gradient and value-label
// font) to the EXISTING gauge objects, WITHOUT tearing the screen down. Every
// theme/colour/gradient/font change routes here. A full rebuild via
// load_current_style() on each switch fragmented LVGL's heap until glyph
// allocation failed, rendering text as boxes; updating styles in place avoids
// that churn entirely (and is far faster).
void apply_theme_colors() {
    if (!gauge_scr || !val_label_int) return;  // gauge UI not built yet
    apply_background(gauge_scr);
    lv_obj_set_style_text_color(val_label_int,   lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(val_label_dec,   lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(mode_label,      lv_color_hex(color_mode_label), 0);
    lv_obj_set_style_text_color(link_icon,       lv_color_hex(color_link_icon), 0);
    lv_obj_set_style_text_color(peak_high_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_color(peak_low_label,  lv_color_hex(color_peak), 0);
    lv_obj_set_style_bg_color(peak_dot,          lv_color_hex(color_peak), 0);
    lv_obj_set_style_line_color(needle_tip,      lv_color_hex(needle_color), 0);
    lv_obj_set_style_border_color(bar,           lv_color_hex(color_low), 0);  // update_ui refreshes live
#if FONT_FIRAMONO_AVAILABLE
    const lv_font_t* font_large = (current_font == 1) ? &firamono_120 : &dseg14_120;
    const lv_font_t* font_mid   = (current_font == 1) ? &firamono_96  : &dseg14_96;
    lv_obj_set_style_text_font(val_label_int, font_large, 0);
    lv_obj_set_style_text_font(val_label_dec, font_mid, 0);
#endif
}

// --- GLOWCRAFT LED PAGE ---
// Top-down Evo silhouette (front at top) with each LED strip drawn as a
// coloured rectangle in its physical position. Layout is a table parallel to
// the strip registry (glowcraft_strips[]), so adding a strip is a one-row edit
// here plus one in the driver. Coordinates are absolute on the 480x480 screen.
struct StripLayout { int16_t x, y, w, h, radius; };
static const StripLayout strip_layout[GC_STRIP_COUNT] = {
  /* GC_STRIP_FRONT           */ { 172,  86, 136, 16, 6 },  // front bar (top)
  /* GC_STRIP_FRONT_BADGE     */ { 222, 110,  36, 26, 6 },  // grille diamond
  /* GC_STRIP_LEFT_HEADLIGHT  */ { 158, 112,  44, 22, 6 },
  /* GC_STRIP_RIGHT_HEADLIGHT */ { 278, 112,  44, 22, 6 },
  /* GC_STRIP_LEFT            */ { 152, 176,  16, 170, 6 }, // left flank (vertical)
  /* GC_STRIP_RIGHT           */ { 312, 176,  16, 170, 6 }, // right flank (vertical)
  /* GC_STRIP_REAR            */ { 172, 398, 136, 16, 6 },  // rear bar (bottom)
  /* GC_STRIP_FRONT_PLATE     */ { 205, 146,  70, 14, 4 },  // placeholder (not installed)
};

static lv_obj_t *strip_widget[GC_STRIP_COUNT] = { nullptr };
static const uint32_t GC_DIM_COLOR = 0x202020;  // strip off / offline

// Builds the GlowCraft screen once. update_glowcraft_page() refreshes the
// strip colours each frame while this page is active.
void build_glowcraft_page() {
  glowcraft_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(glowcraft_scr, lv_color_black(), 0);
  lv_obj_set_style_pad_all(glowcraft_scr, 0, 0);
  lv_obj_clear_flag(glowcraft_scr, LV_OBJ_FLAG_SCROLLABLE);

  // Car body outline behind the strips (drawn first = lowest z-order).
  lv_obj_t *body = lv_obj_create(glowcraft_scr);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, 150, 70);
  lv_obj_set_size(body, 180, 350);
  lv_obj_set_style_radius(body, 40, 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, lv_color_hex(0x404040), 0);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(glowcraft_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_label_set_text(title, "GlowCraft");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  // Strip rectangles.
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    const StripLayout* L = &strip_layout[i];
    lv_obj_t* w = lv_obj_create(glowcraft_scr);
    lv_obj_remove_style_all(w);
    lv_obj_set_pos(w, L->x, L->y);
    lv_obj_set_size(w, L->w, L->h);
    lv_obj_set_style_radius(w, L->radius, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    if (glowcraft_strips[i].installed) {
      lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(w, lv_color_hex(GC_DIM_COLOR), 0);
    } else {
      // Planned-but-not-installed strip: dashed-look outline, no fill.
      lv_obj_set_style_bg_opa(w, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(w, 1, 0);
      lv_obj_set_style_border_color(w, lv_color_hex(0x505050), 0);
    }
    strip_widget[i] = w;
  }
}

// Bench test: drive a slow rolling rainbow into the strip registry as if real
// 0x500+ frames were arriving, so the page can be verified without a live
// GlowCraft. Gated behind test_mode_enabled by the caller.
void glowcraft_test_inject() {
  uint32_t now = millis();
  float phase = now * 0.001f;  // slow cycle
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    if (!glowcraft_strips[i].installed) continue;
    float a = phase + i * 0.8f;  // per-strip hue offset
    glowcraft_strips[i].r = (uint8_t)((sinf(a)          * 0.5f + 0.5f) * 255);
    glowcraft_strips[i].g = (uint8_t)((sinf(a + 2.094f) * 0.5f + 0.5f) * 255);  // +120 deg
    glowcraft_strips[i].b = (uint8_t)((sinf(a + 4.188f) * 0.5f + 0.5f) * 255);  // +240 deg
    glowcraft_strips[i].brightness   = 255;
    glowcraft_strips[i].state        = GC_STATE_ANIMATING;
    glowcraft_strips[i].last_seen_ms = now;  // mark online
  }
}

// Refresh strip widget colours from the live registry. Colours are cached so a
// widget is only invalidated when its colour actually changes (keeps the page
// cheap to render — same discipline as the gauge's needle/label caching).
void update_glowcraft_page() {
  static uint32_t prev_col[GC_STRIP_COUNT] = { 0 };
  static bool init = false;
  if (!init) { for (int i = 0; i < GC_STRIP_COUNT; i++) prev_col[i] = 0xFFFFFFFFu; init = true; }

  uint32_t now = millis();
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    if (!strip_widget[i] || !glowcraft_strips[i].installed) continue;  // placeholders untouched
    uint32_t col = glowcraft_strip_color(i, now);
    uint32_t shown = col ? col : GC_DIM_COLOR;   // dim when off/offline
    if (shown == prev_col[i]) continue;          // unchanged — skip invalidation
    prev_col[i] = shown;
    lv_obj_set_style_bg_color(strip_widget[i], lv_color_hex(shown), 0);
  }
}

// Load the screen matching current_page.
void apply_page() {
  lv_scr_load(current_page == PAGE_GLOWCRAFT ? glowcraft_scr : gauge_scr);
}

void format_secondary(uint8_t metric, char* buf, size_t sz) {
    switch (metric) {
        case 1:  snprintf(buf, sz, "IAT %d\xc2\xb0""C",      HaltechData.intake_air_temp_c); break;
        case 2:  snprintf(buf, sz, "OIL T %.0f\xc2\xb0""C",  HaltechData.oil_temp_c);        break;
        case 3:  snprintf(buf, sz, "FUL T %.0f\xc2\xb0""C",  HaltechData.fuel_temp_c);       break;
        case 4:  snprintf(buf, sz, "FUL P %.1f PSI",          HaltechData.fuel_press_psi);    break;
        case 5:  snprintf(buf, sz, "TPS %d%%",                HaltechData.tps_percent);       break;
        case 6:  snprintf(buf, sz, "LOAD %d%%",               HaltechData.engine_load_pct);   break;
        case 7:  snprintf(buf, sz, "IGN %.1f\xc2\xb0",        HaltechData.ign_timing_deg);    break;
        case 8:  snprintf(buf, sz, "BARO %.1f kPa",           HaltechData.baro_kpa);          break;
        case 9:  snprintf(buf, sz, "%.0f km/h",               HaltechData.vehicle_speed_kph); break;
        case 10: {
          int8_t g = HaltechData.gear;
          if      (g == 0)  snprintf(buf, sz, "GEAR N");
          else if (g <  0)  snprintf(buf, sz, "GEAR R");
          else              snprintf(buf, sz, "GEAR %d", (int)g);
          break;
        }
        default: snprintf(buf, sz, "--"); break;
    }
}

void update_ui(float val, float min, float max, float peak, uint32_t color_hex) {
    // Ring indicator: only update color based on gauge value
    static uint32_t prev_color = 0;
    if (color_hex != prev_color) {
        lv_obj_set_style_border_color(bar, lv_color_hex(color_hex), 0);
        prev_color = color_hex;
    }

    // Peak high / low labels — only call set_text when content actually changes;
    // lv_label_set_text always invalidates regardless of whether text changed.
    static char prev_high[32] = "";
    static char prev_low[32] = "";
    static uint8_t prev_label_mode = 255; // 0=hidden, 1=peak, 2=secondary

    if (peak_hold_enabled) {
        if (prev_label_mode != 1) {
            lv_obj_clear_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 1;
            prev_high[0] = '\0'; prev_low[0] = '\0'; // force text refresh
        }
        char buf[32];
        if (peak_val > -998.0f) snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %.1f", peak_val);
        else                    snprintf(buf, sizeof(buf), LV_SYMBOL_UP " --");
        if (strcmp(buf, prev_high) != 0) {
            lv_label_set_text(peak_high_label, buf);
            strncpy(prev_high, buf, sizeof(prev_high));
        }
        if (peak_low_val < 998.0f) snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %.1f", peak_low_val);
        else                       snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " --");
        if (strcmp(buf, prev_low) != 0) {
            lv_label_set_text(peak_low_label, buf);
            strncpy(prev_low, buf, sizeof(prev_low));
        }
    } else if (secondary_metric != 0) {
        char buf[28];
        format_secondary(secondary_metric, buf, sizeof(buf));
        if (prev_label_mode != 2) {
            lv_obj_set_style_text_align(peak_high_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(peak_high_label, LV_ALIGN_CENTER, 0, -120);
            lv_obj_clear_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 2;
            prev_high[0] = '\0';
        }
        if (strcmp(buf, prev_high) != 0) {
            lv_label_set_text(peak_high_label, buf);
            strncpy(prev_high, buf, sizeof(prev_high));
        }
    } else {
        if (prev_label_mode != 0) {
            lv_obj_add_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 0;
        }
    }
    
    // Update needle tip position based on gauge value
    float angle_start = 135.0f;  // Start angle in degrees (SSW - South-Southwest)
    float angle_range = 270.0f;  // 315 degrees clockwise sweep
    // Path: SSW (112.5°) → South (90°) → N (270°) → E (0°/360°) → SSE (67.5°)
    // Midpoint: 112.5 + 157.5 = 270° (North - 12 o'clock)
    
    // Map value to angle
    float normalized = (val - min) / (max - min);
    normalized = (normalized < 0) ? 0 : (normalized > 1) ? 1 : normalized;  // Clamp to 0-1
    float angle_deg = angle_start + normalized * angle_range;
    
    // Convert to radians
    float angle_rad = angle_deg * M_PI / 180.0f;
    
    // Calculate needle line from 185 to 225px radius to inside of ring
    int center_x = 240;
    int center_y = 240;
    int radius_start = 185;  // Starting radius
    int radius_end = 225;    // Ending radius (tip)
    
    static lv_point_precise_t needle_points[2];
    static lv_point_precise_t prev_needle[2] = {{-9999,-9999},{-9999,-9999}};

    needle_points[0].x = center_x + (int)(radius_start * cosf(angle_rad));
    needle_points[0].y = center_y + (int)(radius_start * sinf(angle_rad));
    needle_points[1].x = center_x + (int)(radius_end   * cosf(angle_rad));
    needle_points[1].y = center_y + (int)(radius_end   * sinf(angle_rad));

    if (needle_points[0].x != prev_needle[0].x || needle_points[0].y != prev_needle[0].y ||
        needle_points[1].x != prev_needle[1].x || needle_points[1].y != prev_needle[1].y) {
        lv_line_set_points(needle_tip, needle_points, 2);
        memcpy(prev_needle, needle_points, sizeof(needle_points));
    }
}

void update_gauge_master() {
    if (text_color != current_applied_text) {
        lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
        lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
        current_applied_text = text_color;
    }

    switch(current_mode) {
      case MODE_BOOST: target_val = HaltechData.boost_psi; break;
      case MODE_AFR: target_val = HaltechData.afr_gas; break;
      case MODE_WATER: target_val = (float)HaltechData.water_temp_c; break;
      case MODE_OIL: target_val = HaltechData.oil_press_psi; break;
    }

    // On a live mode change, jump straight to the new metric instead of sweeping.
    if (snap_displayed) { displayed_val = target_val; snap_displayed = false; }

    // Time-aware smoothing with a per-frame clamp to avoid large jumps
    static unsigned long last_update_ms = 0;
    unsigned long now_ms = millis();
    float dt = last_update_ms ? (now_ms - last_update_ms) / 1000.0f : (1.0f/30.0f);
    last_update_ms = now_ms;
    float delta = target_val - displayed_val;
    if (fabsf(delta) < 0.05f) {
      displayed_val = target_val;
    } else {
      const float smoothing = 0.24f; // lower = smoother/slower
      const float max_rate_per_sec = 40.0f; // units per second maximum change
      float step = delta * smoothing;
      float max_step = max_rate_per_sec * dt;
      if (fabsf(step) > max_step) step = (step > 0) ? max_step : -max_step;
      displayed_val += step;
    }

    if (peak_hold_enabled) {
        if (target_val > peak_val) { peak_val = target_val; peak_timer = millis(); }
        if (target_val < peak_low_val) peak_low_val = target_val;
        if (millis() - peak_timer > PEAK_HOLD_TIME) { peak_val = target_val; peak_low_val = target_val; }
    }

    uint32_t color_hex = color_low;
    lv_color_t c = lv_color_hex(color_low);
    if (current_mode == MODE_BOOST) {
        if(displayed_val < 0) { c = lv_color_hex(color_low); color_hex = color_low; }
        else if(displayed_val < 20) { c = lv_color_hex(color_mid); color_hex = color_mid; }
        else { c = lv_color_hex(color_high); color_hex = color_high; }
    } else if (current_mode == MODE_AFR) {
        if(displayed_val < 10) { c = lv_color_hex(color_low); color_hex = color_low; }
        else if(displayed_val < 15) { c = lv_color_hex(color_mid); color_hex = color_mid; }
        else { c = lv_color_hex(color_high); color_hex = color_high; }
    } else {
        c = lv_color_hex(color_mid); color_hex = color_mid;
    }

    int i_part = (int)displayed_val;
    int d_part = abs((int)((displayed_val - i_part) * 10));
    char b1[16]; snprintf(b1, sizeof(b1), "%d", i_part);
    char b2[16]; snprintf(b2, sizeof(b2), ".%d", d_part);
    static char prev_b1[16] = ""; static char prev_b2[16] = "";
    bool text_changed = false;
    if (strcmp(prev_b1, b1) != 0) {
      lv_label_set_text(val_label_int, b1);
      strncpy(prev_b1, b1, sizeof(prev_b1));
      text_changed = true;
    }
    if (strcmp(prev_b2, b2) != 0) {
      lv_label_set_text(val_label_dec, b2);
      strncpy(prev_b2, b2, sizeof(prev_b2));
      text_changed = true;
    }
    if (text_changed) {
      // Measure the new strings directly with the font — same result as the old
      // lv_obj_update_layout()+lv_obj_get_width() (label width == text width for
      // auto-sized labels) but without forcing a full layout pass per value tick,
      // and it can never read a stale cached width.
      lv_point_t sz;
      lv_text_get_size(&sz, b1, lv_obj_get_style_text_font(val_label_int, LV_PART_MAIN),
                       lv_obj_get_style_text_letter_space(val_label_int, LV_PART_MAIN),
                       0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int int_w = sz.x;
      lv_text_get_size(&sz, b2, lv_obj_get_style_text_font(val_label_dec, LV_PART_MAIN),
                       lv_obj_get_style_text_letter_space(val_label_dec, LV_PART_MAIN),
                       0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int dec_w = sz.x;
      // Fixed anchor: integer right-edge and decimal left-edge both at screen_centre + ANCHOR_OFS
      const int ANCHOR_OFS = 44;
      lv_obj_align(val_label_int, LV_ALIGN_CENTER, ANCHOR_OFS - int_w / 2, 5);
      lv_obj_align(val_label_dec, LV_ALIGN_CENTER, ANCHOR_OFS + dec_w / 2, 5);
    }

    float min = RANGES[current_mode][0];
    float max = RANGES[current_mode][1];
    
    update_ui(displayed_val, min, max, peak_val, color_hex);
}

// --- CAN BUS ---
uint16_t get_uint16_be(uint8_t *data, int offset) { return (data[offset] << 8) | data[offset + 1]; }

void process_can_queue_task(void *arg) {
  twai_message_t message;
  while (1) {
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
      switch (message.identifier) {
        case 0x360: {
          HaltechData.rpm          = get_uint16_be(message.data, 0);
          uint16_t raw_map         = get_uint16_be(message.data, 2);
          HaltechData.boost_psi    = (raw_map * 0.1 - 101.3) * 0.145038;
          uint16_t raw_tps         = get_uint16_be(message.data, 4);
          HaltechData.tps_percent  = raw_tps / 10;
          uint16_t raw_load        = get_uint16_be(message.data, 6);
          HaltechData.engine_load_pct = raw_load / 10;
          break;
        }
        case 0x361: {
          uint16_t raw_oil         = get_uint16_be(message.data, 2);
          HaltechData.oil_press_psi = (raw_oil * 0.1) * 0.145038;
          // Ignition timing: raw = (degrees + 720) * 10, so degrees = raw/10 - 720
          uint16_t raw_ign         = get_uint16_be(message.data, 4);
          HaltechData.ign_timing_deg = (raw_ign / 10.0f) - 720.0f;
          uint16_t raw_baro        = get_uint16_be(message.data, 6);
          HaltechData.baro_kpa     = raw_baro * 0.1f;
          break;
        }
        case 0x362: {
          uint16_t raw_coolant     = get_uint16_be(message.data, 0);
          HaltechData.water_temp_c = (raw_coolant / 10) - 273;
          uint16_t raw_iat         = get_uint16_be(message.data, 2);
          HaltechData.intake_air_temp_c = (raw_iat / 10) - 273;
          uint16_t raw_fuel_temp   = get_uint16_be(message.data, 4);
          HaltechData.fuel_temp_c  = (raw_fuel_temp / 10.0f) - 273.0f;
          uint16_t raw_oil_temp    = get_uint16_be(message.data, 6);
          HaltechData.oil_temp_c   = (raw_oil_temp / 10.0f) - 273.0f;
          break;
        }
        case 0x363: {
          uint16_t raw_fuel        = get_uint16_be(message.data, 0);
          HaltechData.fuel_press_psi = (raw_fuel * 0.1f) * 0.145038f;
          break;
        }
        case 0x365: {
          uint16_t raw_spd         = get_uint16_be(message.data, 0);
          HaltechData.vehicle_speed_kph = raw_spd * 0.1f;
          break;
        }
        case 0x366: {
          HaltechData.gear = (int8_t)message.data[0];
          break;
        }
        case 0x368: {
          uint16_t raw_lambda      = get_uint16_be(message.data, 0);
          HaltechData.afr_gas      = (raw_lambda / 1000.0) * 14.7;
          break;
        }
        case 0x3E4: {
          // Rotary Trim 3 (byte 6, 4-position rotary returning 0-3). When theme
          // sync is enabled it selects the live colour slot — debounced so only
          // an actual position change rebuilds the theme. The heavy LVGL work is
          // deferred to loop() via flag_theme_update (this runs in the CAN task).
          if (message.data_length_code > 6) {
            int pos = message.data[6];
            if (pos >= 0 && pos < THEME_SLOTS && pos != last_trimpot3) {
              last_trimpot3 = pos;
              if (trimpot_theme_sync) {
                active_theme = (uint8_t)pos;
                theme_to_globals(active_theme);
                flag_theme_update = true;
              }
            }
          }
          break;
        }
        default:
          // GlowCraft strip-status frames (0x500+) — decoded in their own module.
          glowcraft_decode(&message);
          break;
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void receive_can_task(void *arg) {
  static unsigned long last_recover_ms = 0;
  while (1) {
    twai_message_t message;
    // Non-blocking drain of everything the TWAI RX queue currently holds, bounded
    // so a flooded bus can't starve the same-core LVGL loop. Previously we pulled
    // a single frame per 1 ms tick, so bursts overflowed the (shallow) hardware RX
    // queue and dropped frames — including 5 Hz ones like Rotary Trim 3 (0x3E4),
    // which made theme-sync miss knob changes.
    int drained = 0;
    while (drained < 24) {
      esp_err_t err = twai_receive(&message, 0);
      if (err == ESP_OK) {
        xQueueSend(canMsgQueue, &message, 0);
        drained++;
        continue;
      }
      if (err != ESP_ERR_TIMEOUT) {
        // Actual bus error (not just an empty queue) — recover, rate-limited to 5s.
        unsigned long now = millis();
        if (now - last_recover_ms > 5000) {
          last_recover_ms = now;
          canbus_recover();
        }
      }
      break;  // queue drained (TIMEOUT) or bus error — done for this tick
    }
    vTaskDelay(pdMS_TO_TICKS(1));  // yield to the LVGL loop (same core, lower prio)
  }
}

void setup() {
  Serial.begin(115200);
  // Always log reset reason — most useful single diagnostic
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("\n\n=== BOOT: %s ===\n", reset_reason_str(rr));
  Serial.printf("Heap: %u free / %u total\n", ESP.getFreeHeap(), ESP.getHeapSize());
  Serial.printf("PSRAM: %u free / %u total\n", ESP.getFreePsram(), ESP.getPsramSize());

  // After a brownout the ST7701's internal LDOs may still be settling.
  // Hold RESET low longer and wait before initialising to guarantee a clean panel state.
  bool was_brownout = (rr == ESP_RST_BROWNOUT);
  if (was_brownout) {
    Serial.println("[DISP] Brownout detected — extending display reset hold");
    i2c_init();
    tca9554pwr_init(0x00);
    set_exio(EXIO_PIN1, Low);   // Hold RESET low for 300 ms instead of 50 ms
    vTaskDelay(pdMS_TO_TICKS(300));
    set_exio(EXIO_PIN1, High);
    vTaskDelay(pdMS_TO_TICKS(500)); // Extra settle time for panel LDOs
  }

  drivers_init();
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0); 

  // Generate unique default name from eFuse chip ID (no WiFi required)
  uint64_t chipid = ESP.getEfuseMac();
  char defaultName[20];
  snprintf(defaultName, sizeof(defaultName), "Gauge-%04X", (uint16_t)(chipid >> 32));

  preferences.begin("gauge", false);
  current_mode = (GaugeMode)preferences.getInt("mode", 0);
  text_color = preferences.getUInt("ct", 0xFFD700);
  color_low  = preferences.getUInt("cl", 0x2196F3);
  color_mid  = preferences.getUInt("cm", 0x4CAF50);
  color_high = preferences.getUInt("ch", 0xF44336);
  color_background = preferences.getUInt("cbg", 0x000000);
  color_mode_label = preferences.getUInt("cml", 0x969696);
  color_link_icon = preferences.getUInt("cli", 0x00C851);
  needle_color = preferences.getUInt("cn", 0xFF6600);
  color_peak = preferences.getUInt("cp", 0xFFFFFF);
  color_background2 = preferences.getUInt("cbg2", 0x000000);
  color_background3 = preferences.getUInt("cbg3", 0x000000);
  bg_grad_type = preferences.getUChar("cgt", 0);
  bg_grad_stops = preferences.getUChar("cgs", 2);
  bg_grad_angle = preferences.getUShort("cga", 0);
  current_brightness = preferences.getInt("bright", 40);
  peak_hold_enabled = preferences.getBool("peak", true);
  debug_mode_enabled = preferences.getBool("dbg", false);
  current_font = (uint8_t)preferences.getUInt("font", 0);
  device_name = preferences.getString("devname", defaultName);
  secondary_metric = (uint8_t)preferences.getUInt("sm", 0);
  current_page = (DisplayPage)preferences.getUInt("page", 0);
  active_theme = (uint8_t)preferences.getUInt("atheme", 0);
  trimpot_theme_sync = preferences.getBool("tpsync", false);
  // Crash-safe boot flag: set false before risky rendering, true once we light
  // the panel. If it's still false here, the previous boot crashed/hung mid-render.
  bool last_boot_completed = preferences.getBool("bootok", true);
  preferences.end();
  if (active_theme >= THEME_SLOTS) active_theme = 0;

  // Build the four theme slots (slot 0 = the legacy theme just loaded above),
  // then make the saved active slot live before the first style pass.
  load_all_themes();
  theme_to_globals(active_theme);

  // --- Crash-safe gradient guard (auto-unbrick) ---
  // A heap-heavy gradient (radial/conical) can fail to render and crash before
  // the panel ever lights. Because that happens before loop() runs, OTA can't
  // recover it. If the last boot didn't complete, disable the active slot's
  // gradient and persist it so we boot clean instead of crash-looping.
  if (!last_boot_completed) {
    Serial.println("[SAFE] Previous boot did not complete — disabling gradient (safe mode)");
    bg_grad_type = 0;
    themes[active_theme].bg_grad_type = 0;
    preferences.begin("gauge", false);
    preferences.putUChar("cgt", 0);
    char k[8]; theme_key(k, active_theme, "gt"); preferences.putUChar(k, 0);
    preferences.end();
  }
  // Mark boot as in-progress; cleared at the end of setup() once we've rendered
  // and lit the backlight. A crash before then leaves this false -> safe mode.
  preferences.begin("gauge", false);
  preferences.putBool("bootok", false);
  preferences.end();

  gauge_scr = lv_scr_act();   // the default screen holds the gauge UI
  setup_wifi();               // bring up AP + web server + OTA BEFORE any risky
                              // rendering so a bad style can never lock out OTA
  load_current_style();
  build_glowcraft_page();
  apply_page();               // load whichever page was last selected

  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);

  // Render a few frames before enabling backlight — ensures the framebuffer
  // contains valid content before it's visible, preventing startup corruption.
  // Pump OTA/web here too so recovery stays possible even if a render stalls.
  for (int i = 0; i < 5; i++) { lv_timer_handler(); ArduinoOTA.handle(); server.handleClient(); }
  set_backlight(current_brightness);

  // Boot fully succeeded (rendered + backlit) — clear the in-progress flag so the
  // next boot is treated as clean. If we'd crashed above, this stays false.
  preferences.begin("gauge", false);
  preferences.putBool("bootok", true);
  preferences.end();

  // Post-setup memory diagnostics — internal heap is the scarce resource
  // (WiFi + LVGL draw buffers + lwip all draw on it). Watch this on bench.
  Serial.printf("[BOOT] Setup complete. Internal heap: %u free (min ever %u, largest block %u). PSRAM: %u free\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                ESP.getFreePsram());
}

void loop() {
  unsigned long lvgl_t = millis();
  lv_timer_handler();
  if (show_perf_stats) perf_lvgl_ms = millis() - lvgl_t;
  server.handleClient();
  ArduinoOTA.handle();
  
  // --- FLAG HANDLERS ---
  if (reboot_at_ms && (int32_t)(millis() - reboot_at_ms) >= 0) ESP.restart();
  if (flag_theme_update) {
      flag_theme_update = false;
      apply_theme_colors();   // in-place colour/gradient/font update (no teardown)
  }
  if (flag_persist_theme) {
      // A theme pushed in via ESP-NOW ("Apply to ALL"): fold it into the active
      // slot and write to NVS here, in loop context, not in the recv callback.
      flag_persist_theme = false;
      globals_to_theme(active_theme); persist_theme(active_theme);
  }
  if (flag_bright_update) {
      flag_bright_update = false;
      if (pending_brightness >= 0) {  // remote (ESP-NOW) change: apply + persist here
          current_brightness = pending_brightness;
          pending_brightness = -1;
          preferences.begin("gauge", false); preferences.putInt("bright", current_brightness); preferences.end();
      }
      set_backlight(current_brightness);
  }
  if (flag_new_peer) {
      flag_new_peer = false;
      lv_obj_clear_flag(link_icon, LV_OBJ_FLAG_HIDDEN); 
  }
  if (flag_stats_update) {
      flag_stats_update = false;
      if(show_perf_stats) lv_obj_clear_flag(perf_label, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (flag_page_update) {
      flag_page_update = false;
      apply_page();
  }
  if (flag_mode_update) {
      flag_mode_update = false;
      if (pending_mode >= 0) {  // remote (ESP-NOW) change: apply + persist here
          current_mode = (GaugeMode)pending_mode;
          pending_mode = -1;
          preferences.begin("gauge", false); preferences.putInt("mode", (int)current_mode); preferences.end();
      }
      lv_label_set_text(mode_label, MODE_NAMES[current_mode]);
      // Fresh state for the new metric: drop stale peaks and snap the needle so
      // it doesn't sweep across the dial from the old mode's value.
      peak_val = -999.0f; peak_low_val = 999.0f; peak_timer = millis();
      snap_displayed = true;
  }

  // --- STATS LOGIC ---
  if (show_perf_stats) {
      perf_frames++;
      if (millis() - perf_last_time >= 1000) {
          perf_fps = perf_frames;
          perf_frames = 0;
          perf_last_time = millis();
          lv_label_set_text_fmt(perf_label, "FPS:%d LV:%d R:%d UI:%d", perf_fps, perf_lvgl_ms, lvgl_render_ms, perf_frame_ms);
      }
  }

  if (millis() - last_broadcast > 2000) {
      last_broadcast = millis();
      broadcast_presence();
  }

  if (debug_mode_enabled && millis() - debug_last_print > DEBUG_INTERVAL_MS) {
      debug_last_print = millis();
      twai_status_info_t can_status;
      twai_get_status_info(&can_status);
      Serial.printf("[DBG] Heap:%u PSRAM:%u | CAN state:%d tx_err:%u rx_err:%u | Mode:%s Val:%.2f\n",
          ESP.getFreeHeap(),
          ESP.getFreePsram(),
          (int)can_status.state,
          can_status.tx_error_counter,
          can_status.rx_error_counter,
          MODE_NAMES[current_mode],
          displayed_val);
      Serial.printf("[DBG] Tasks - RxCAN:%u ProcCAN:%u Loop:%u\n",
          uxTaskGetStackHighWaterMark(NULL),  // current task (loop)
          0, 0);  // individual task handles not stored, but loop stack is most relevant
  }
  
  if (millis() - last_data_time > 16) {
      unsigned long start = millis();
      last_data_time = start;
      if (current_page == PAGE_GLOWCRAFT) {
          if (test_mode_enabled) glowcraft_test_inject();
          update_glowcraft_page();
      } else {
      if (test_mode_enabled) {
          static float t=0; t+=0.05;
          HaltechData.boost_psi = -15 + (sin(t) + 1) * 22.5;
          HaltechData.afr_gas = 8 + (sin(t*0.5) + 1) * 7.0;
          HaltechData.water_temp_c = 50 + (int)((sin(t*0.3) + 1) * 35.0);
          HaltechData.oil_press_psi = 10 + (sin(t*0.7) + 1) * 45.0;
          HaltechData.intake_air_temp_c  = 25   + (int)((sin(t*0.20) + 1.0) * 22.5);
          HaltechData.oil_temp_c         = 80.0f + (sin(t*0.15f) + 1.0f) * 20.0f;
          HaltechData.fuel_temp_c        = 35.0f + (sin(t*0.18f) + 1.0f) * 10.0f;
          HaltechData.fuel_press_psi     = 40.0f + (sin(t*0.40f) + 1.0f) * 10.0f;
          HaltechData.tps_percent        = (int)((sin(t*0.60) + 1.0) * 50.0);
          HaltechData.engine_load_pct    = (int)((sin(t*0.55) + 1.0) * 50.0);
          HaltechData.ign_timing_deg     = 15.0f + (sin(t*0.30f) + 1.0f) * 10.0f;
          HaltechData.baro_kpa           = 101.3f;
          HaltechData.vehicle_speed_kph  = (sin(t*0.10f) + 1.0f) * 80.0f;
          HaltechData.gear               = (int8_t)((int)((sin(t*0.08) + 1.0) * 3.0) + 1);
      }
      update_gauge_master();
      }

      if(show_perf_stats) perf_frame_ms = millis() - start;
  }
  yield();
}