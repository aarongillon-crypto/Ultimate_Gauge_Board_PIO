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

LV_FONT_DECLARE(dseg14_60);
LV_FONT_DECLARE(dseg14_96);
LV_FONT_DECLARE(dseg14_120);

#define FONT_FIRAMONO_AVAILABLE 1
#if FONT_FIRAMONO_AVAILABLE
LV_FONT_DECLARE(firamono_96);
LV_FONT_DECLARE(firamono_120);
#endif
// LV_IMG_DECLARE(gauge_bg);

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

String device_name = "Gauge";  // loaded from NVS, used for AP SSID and BLE name
uint32_t text_color = 0xFFD700;
uint32_t color_low = 0x2196F3, color_mid = 0x4CAF50, color_high = 0xF44336;
uint32_t color_mode_label = 0x969696; // Mode label (gray)
uint32_t color_link_icon = 0x00C851; // Connectivity icon (green)
uint32_t needle_color = 0xFF6600;     // Needle color (orange)
uint32_t color_peak = 0xFFFFFF;      // Peak stripe (white)
uint32_t color_background = 0x000000; // Screen background (black)
uint32_t current_applied_text = 0;
int current_brightness = 40;
uint8_t current_font = 0;
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
volatile bool flag_reboot = false;
volatile bool flag_theme_update = false; 
volatile bool flag_bright_update = false;
volatile bool flag_stats_update = false;
volatile bool flag_page_update = false;

#define WIFI_CHANNEL 1
typedef struct __attribute__((packed)) { 
    uint8_t type; 
    int mode; 
    uint32_t c1, c2, c3, c4; 
    int value; 
} EspNowPacket;

typedef struct { uint8_t mac[6]; int mode; unsigned long last_seen; } PeerGauge;
PeerGauge fleet[10]; int fleet_count = 0;

lv_obj_t *main_scr;
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

bool receiving_data = false;
volatile bool data_ready = false;

void drivers_init() {
  i2c_init(); tca9554pwr_init(0x00); lcd_init(); canbus_init(); glowcraft_init(); lvgl_init();
}

void log_msg(String msg) { Serial.println(msg); }

void update_peer_list(const uint8_t *mac, int mode) {
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
    flag_new_peer = true; 
  }
}

void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t *incomingData, int len) {
  const uint8_t* mac = info->src_addr;
  if (len != sizeof(EspNowPacket)) return;

  // Ignore our own packets echoed back via the AP interface (check both STA and AP MACs)
  uint8_t myStaMac[6], myApMac[6];
  WiFi.macAddress(myStaMac);
  WiFi.softAPmacAddress(myApMac);
  if (memcmp(mac, myStaMac, 6) == 0 || memcmp(mac, myApMac, 6) == 0) return;

  EspNowPacket *pkt = (EspNowPacket *)incomingData;

  Serial.printf("[ESP-NOW] type=%d mode=%d from %02X:%02X:%02X:%02X:%02X:%02X to %02X:%02X:%02X:%02X:%02X:%02X\n",
    pkt->type, pkt->mode,
    mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
    info->des_addr[0], info->des_addr[1], info->des_addr[2],
    info->des_addr[3], info->des_addr[4], info->des_addr[5]);

  if (pkt->type == 1) {
    update_peer_list(mac, pkt->mode);
  }
  else if (pkt->type == 2) {
    preferences.begin("gauge", false); preferences.putInt("mode", pkt->mode); preferences.end();
    flag_reboot = true;
  }
  else if (pkt->type == 3) { 
    text_color = pkt->c1; color_low = pkt->c2; color_mid = pkt->c3; color_high = pkt->c4;
    preferences.begin("gauge", false);
    preferences.putUInt("ct", text_color); preferences.putUInt("cl", color_low);
    preferences.putUInt("cm", color_mid); preferences.putUInt("ch", color_high);
    preferences.end();
    flag_theme_update = true; 
  }
  else if (pkt->type == 4) { 
    test_mode_enabled = (pkt->value == 1);
  }
  else if (pkt->type == 5) { 
    current_brightness = pkt->value;
    preferences.begin("gauge", false); preferences.putInt("bright", current_brightness); preferences.end();
    flag_bright_update = true;
  }
  else if (pkt->type == 6) { 
    show_perf_stats = (pkt->value == 1);
    flag_stats_update = true;
  }
  else if (pkt->type == 7) { 
    // UI Colors broadcast
    color_background = pkt->c1;
    color_mode_label = pkt->c2;
    color_link_icon = pkt->c3;
    needle_color = pkt->c4;
    color_peak = (uint32_t)pkt->value;
    preferences.begin("gauge", false);
    preferences.putUInt("cbg", color_background);
    preferences.putUInt("cml", color_mode_label);
    preferences.putUInt("cli", color_link_icon);
    preferences.putUInt("cn", needle_color);
    preferences.putUInt("cp", color_peak);
    preferences.end();
    flag_theme_update = true;
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
String macToString(uint8_t *mac) {
  char buf[18]; snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]); return String(buf);
}

void handleRoot() {
  String html;
  html.reserve(3500); // pre-allocate to avoid repeated heap reallocs under PSRAM pressure
  html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;padding:10px;background:#222;color:#fff;} .card{background:#333;margin:10px;padding:15px;border-radius:10px;} button{font-size:16px;width:45%;padding:10px;margin:5px;border:none;border-radius:5px;cursor:pointer;} .btn-b{background:#0099ff;color:white;} .btn-a{background:#00cc66;color:white;} .btn-w{background:#ff9900;color:white;} .btn-o{background:#cc3300;color:white;} input[type=color]{width:50px;height:40px;border:none;vertical-align:middle;margin:5px;} label{display:inline-block;width:60px;text-align:right;} input[type=range]{width:60%;vertical-align:middle;}</style></head><body>";
  html += "<h1>" + device_name + "</h1>";
  html += "<p>Peers Found: " + String(fleet_count) + "</p>";
  html += "<div class='card'><h3>DEVICE NAME</h3><form action='/name' method='get'><input type='text' name='n' value='" + device_name + "' maxlength='20' style='font-size:16px;padding:8px;width:70%;border-radius:5px;border:none;'> <button style='width:auto;background:#555;color:white;'>Rename</button></form><small style='color:#aaa'>Used as WiFi AP name (Haltech-[name]) and BLE name. Restarts device.</small></div>";
  
  html += "<div class='card'><h3>DYNAMIC ELEMENTS</h3><form action='/theme' method='get'><div><label>Text:</label><input type='color' name='ct' value='" + colorToHex(text_color) + "'></div><div><label>Low:</label><input type='color' name='cl' value='" + colorToHex(color_low) + "'></div><div><label>Mid:</label><input type='color' name='cm' value='" + colorToHex(color_mid) + "'></div><div><label>High:</label><input type='color' name='ch' value='" + colorToHex(color_high) + "'></div><button style='width:auto;margin-top:10px;background:#d32f2f;color:white;'>Apply to ALL</button></form></div>";



  html += "<div class='card'><h3>STATIC ELEMENTS</h3><form action='/uicolors' method='get'><div><label>Background:</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div><div><label>Mode Label:</label><input type='color' name='cml' value='" + colorToHex(color_mode_label) + "'></div><div><label>Link Icon:</label><input type='color' name='cli' value='" + colorToHex(color_link_icon) + "'></div><div><label>Needle:</label><input type='color' name='cn' value='" + colorToHex(needle_color) + "'></div><div><label>Peak Stripe:</label><input type='color' name='cp' value='" + colorToHex(color_peak) + "'></div><button style='width:auto;margin-top:10px;background:#2196F3;color:white;'>Apply to ALL</button></form></div>";

  html += "<div class='card'><h3>GLOBAL CONTROLS</h3><form action='/bright' method='get'><label>Brightness: </label><input type='range' name='b' min='10' max='100' value='" + String(current_brightness) + "' onchange='this.form.submit()'></form>";
  html += "<a href='/test?t=" + String(!test_mode_enabled) + "'><button class='btn'>Test Mode: " + String(test_mode_enabled?"ON":"OFF") + "</button></a>";
  html += "<br><a href='/stats?s=" + String(!show_perf_stats) + "'><button class='btn'>Stats: " + String(show_perf_stats?"ON":"OFF") + "</button></a>";
  html += "<br><a href='/debug?d=" + String(!debug_mode_enabled) + "'><button class='btn' style='background:" + String(debug_mode_enabled?"#e65100":"#555") + "'>Serial Debug: " + String(debug_mode_enabled?"ON":"OFF") + "</button></a>";
  html += "<br><a href='/font?f=" + String(current_font == 0 ? 1 : 0) + "'><button class='btn' style='background:#4a148c'>Font: " + String(current_font == 0 ? "DSEG14" : "Fira Mono") + "</button></a>";
  html += "<br><a href='/preview'><button class='btn' style='background:#1a237e'>Live Preview</button></a>";
  html += "<br><a href='/ota'><button class='btn' style='background:#37474f'>OTA Firmware Update</button></a>";
  html += "</div>";

  html += "<div class='card'><h3>DISPLAY PAGE</h3>";
  html += "<a href='/page?pg=0'><button class='btn' style='background:" + String(current_page==PAGE_GAUGE?"#00cc66":"#555") + "'>Gauge</button></a>";
  html += "<a href='/page?pg=1'><button class='btn' style='background:" + String(current_page==PAGE_GLOWCRAFT?"#00cc66":"#555") + "'>GlowCraft LEDs</button></a>";
  html += "</div>";

  html += "<div class='card'><h3>LOCAL GAUGE</h3>";
  // PEAK TOGGLE
  html += "<a href='/peak?p=" + String(!peak_hold_enabled) + "'><button class='btn'>Peak Hold: " + String(peak_hold_enabled?"ON":"OFF") + "</button></a><br>";

  // SECONDARY METRIC (shown when peak hold is off)
  html += "<div style='margin-top:10px'>";
  html += "<p style='margin:4px 0;color:#aaa;font-size:13px'>Secondary metric (when Peak Hold OFF):</p>";
  html += "<form action='/secondary' method='get' style='display:flex;gap:8px;align-items:center;flex-wrap:wrap'>";
  html += "<select name='sm' style='font-size:14px;padding:5px;border-radius:5px;flex:1'>";
  for (int i = 0; i < SECONDARY_COUNT; i++) {
    html += "<option value='" + String(i) + "'";
    if (secondary_metric == i) html += " selected";
    html += ">" + String(SECONDARY_NAMES[i]) + "</option>";
  }
  html += "</select>";
  html += "<button type='submit' style='background:#5c6bc0;color:white;border:none;padding:6px 14px;border-radius:5px;font-size:14px'>Set</button>";
  html += "</form></div>";
  
  html += "<p>Mode: <strong>" + String(MODE_NAMES[current_mode]) + "</strong></p>";
  html += "<a href='/set?mode=0'><button class='btn-b'>Boost</button></a>";
  html += "<a href='/set?mode=1'><button class='btn-a'>AFR</button></a>";
  html += "<a href='/set?mode=2'><button class='btn-w'>Water</button></a>";
  html += "<a href='/set?mode=3'><button class='btn-o'>Oil</button></a>";
  html += "</div>";
  
  if (fleet_count > 0) {
    html += "<h3>REMOTE GAUGES</h3>";
    for(int i=0; i<fleet_count; i++) {
        if (millis() - fleet[i].last_seen < 10000) {
            String macStr = "";
            for(int j=0; j<6; j++) { if(j>0) macStr += ":"; char buf[3]; sprintf(buf, "%02X", fleet[i].mac[j]); macStr += buf; }
            String macClean = macStr; macClean.replace(":", ""); 
            html += "<div class='card'><h4>Gauge " + macClean.substring(9) + "</h4><p>" + String(MODE_NAMES[fleet[i].mode]) + "</p><a href='/rem?mac=" + macClean + "&mode=0'><button class='btn-b'>Boost</button></a><a href='/rem?mac=" + macClean + "&mode=1'><button class='btn-a'>AFR</button></a><a href='/rem?mac=" + macClean + "&mode=2'><button class='btn-w'>Water</button></a><a href='/rem?mac=" + macClean + "&mode=3'><button class='btn-o'>Oil</button></a></div>";
        }
    }
  }
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
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handleSet() {
    if (server.hasArg("mode")) {
        int m = server.arg("mode").toInt();
        Serial.printf("[HTTP] handleSet: local mode -> %d\n", m);
        preferences.begin("gauge", false); preferences.putInt("mode", m); preferences.end();
        // Send response BEFORE restarting — otherwise browser gets a connection reset,
        // retries the GET, and if the phone switches to the slave's AP during reboot
        // the slave's web server receives and processes the same /set?mode=X request.
        server.sendHeader("Location", "/");
        server.send(303);
        server.client().flush();
        delay(300);
        ESP.restart();
    }
}
void handleTest() {
    if (server.hasArg("t")) test_mode_enabled = server.arg("t").toInt();
    EspNowPacket pkt = {}; pkt.type = 4; pkt.value = test_mode_enabled?1:0; broadcast_packet(&pkt);
    server.sendHeader("Location", "/"); server.send(303);
}
void handleStats() {
    if (server.hasArg("s")) show_perf_stats = server.arg("s").toInt();
    EspNowPacket pkt = {}; pkt.type = 6; pkt.value = show_perf_stats?1:0; broadcast_packet(&pkt);
    flag_stats_update = true;
    server.sendHeader("Location", "/"); server.send(303);
}
void handleDebug() {
    if (server.hasArg("d")) {
        debug_mode_enabled = server.arg("d").toInt();
        preferences.begin("gauge", false);
        preferences.putBool("dbg", debug_mode_enabled);
        preferences.end();
    }
    server.sendHeader("Location", "/"); server.send(303);
}
void handleBright() {
    if (server.hasArg("b")) {
        int b = server.arg("b").toInt();
        current_brightness = b; set_backlight(b);
        preferences.begin("gauge", false); preferences.putInt("bright", b); preferences.end();
        EspNowPacket pkt = {}; pkt.type = 5; pkt.value = b; broadcast_packet(&pkt);
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handlePeak() {
    if (server.hasArg("p")) {
        peak_hold_enabled = server.arg("p").toInt();
        if (peak_hold_enabled) { peak_val = -999.0f; peak_low_val = 999.0f; }
        preferences.begin("gauge", false); preferences.putBool("peak", peak_hold_enabled); preferences.end();
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handleSecondary() {
    if (server.hasArg("sm"))
        secondary_metric = (uint8_t)constrain(server.arg("sm").toInt(), 0, SECONDARY_COUNT - 1);
    preferences.begin("gauge", false);
    preferences.putUInt("sm", secondary_metric);
    preferences.end();
    server.sendHeader("Location", "/"); server.send(303);
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
    server.sendHeader("Location", "/"); server.send(303);
}
void handleFont() {
    if (server.hasArg("f")) {
        uint8_t f = (uint8_t)server.arg("f").toInt();
        if (f <= 1) {
            current_font = f;
            preferences.begin("gauge", false);
            preferences.putUInt("font", current_font);
            preferences.end();
            flag_theme_update = true;
        }
    }
    server.sendHeader("Location", "/"); server.send(303);
}
// Capture the live RGB framebuffer and serve it as a BMP image.
// The RGB panel keeps 2 PSRAM framebuffers; we grab whichever is current.
void handleSnapshot() {
    void *fb0 = nullptr, *fb1 = nullptr;
    if (esp_lcd_rgb_panel_get_frame_buffer(panel_handle, 2, &fb0, &fb1) != ESP_OK || fb0 == nullptr) {
        server.send(500, "text/plain", "Framebuffer unavailable");
        return;
    }

    const int W = 480, H = 480;
    const int row_stride = (W * 3 + 3) & ~3;  // BMP rows padded to 4 bytes
    const int file_size  = 54 + row_stride * H;

    uint8_t* bmp = (uint8_t*)heap_caps_malloc(file_size, MALLOC_CAP_SPIRAM);
    if (!bmp) {
        server.send(500, "text/plain", "Out of PSRAM");
        return;
    }

    // --- BMP file header (14 bytes) ---
    memset(bmp, 0, 54);
    bmp[0] = 'B'; bmp[1] = 'M';
    *(uint32_t*)(bmp + 2)  = file_size;
    *(uint32_t*)(bmp + 10) = 54;          // pixel data offset
    // --- DIB header (40 bytes) ---
    *(uint32_t*)(bmp + 14) = 40;          // header size
    *(int32_t*) (bmp + 18) = W;
    *(int32_t*) (bmp + 22) = -H;          // negative = top-down row order
    *(uint16_t*)(bmp + 26) = 1;           // colour planes
    *(uint16_t*)(bmp + 28) = 24;          // bits per pixel (RGB888)
    *(uint32_t*)(bmp + 34) = row_stride * H;

    // --- Convert RGB565 framebuffer → RGB888 BMP pixel data ---
    const uint16_t* src = (const uint16_t*)fb0;
    uint8_t* dst = bmp + 54;
    for (int y = 0; y < H; y++) {
        for (int x = 0; x < W; x++) {
            uint16_t px = src[y * W + x];
            dst[x*3 + 0] = (px & 0x1F)         << 3;  // B
            dst[x*3 + 1] = ((px >> 5)  & 0x3F) << 2;  // G
            dst[x*3 + 2] = ((px >> 11) & 0x1F) << 3;  // R
        }
        dst += row_stride;
    }

    server.setContentLength(file_size);
    server.send(200, "image/bmp", "");
    WiFiClient client = server.client();
    size_t sent = 0;
    while (sent < (size_t)file_size) {
        size_t chunk = min((size_t)4096, (size_t)file_size - sent);
        client.write(bmp + sent, chunk);
        sent += chunk;
    }
    heap_caps_free(bmp);
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
        "var iv=setInterval(reload,2000);"  // auto-refresh every 2s
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
    if (ok) { delay(1000); ESP.restart(); }
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
            server.client().flush();
            delay(300);
            ESP.restart(); // Restart to apply new AP SSID and BLE name
        } else {
            server.send(400, "text/plain", "Name must be 1-20 characters");
        }
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
        server.sendHeader("Location", "/"); server.send(303);
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
        server.sendHeader("Location", "/"); server.send(303);
    }
}

void setup_wifi() {
  WiFi.mode(WIFI_AP_STA);

  esp_wifi_set_promiscuous(true);
  esp_wifi_set_channel(WIFI_CHANNEL, WIFI_SECOND_CHAN_NONE);
  esp_wifi_set_promiscuous(false);

  char ssid[32];
  snprintf(ssid, sizeof(ssid), "Haltech-%s", device_name.c_str());
  WiFi.softAP(ssid, NULL, WIFI_CHANNEL);

  // Reduce WiFi power to minimize RF interference with display PSRAM bus
  esp_wifi_set_max_tx_power(34); // Reduce to ~8.5dBm to minimise PSRAM bus contention during TX bursts

  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(OnDataRecv);
  
  server.on("/", handleRoot);
  server.on("/theme", handleTheme); server.on("/set", handleSet); server.on("/rem", handleRemote); server.on("/name", handleName);
  server.on("/bright", handleBright); server.on("/test", handleTest); server.on("/stats", handleStats); server.on("/debug", handleDebug);
  server.on("/peak", handlePeak); server.on("/uicolors", handleUIColors); server.on("/font", handleFont);
  server.on("/secondary", handleSecondary); server.on("/page", handlePage);
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
}

// --- UI ---
void common_label_setup() {
  val_label_int = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_int, true, 0);

  val_label_dec = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_dec, true, 0);

  // Reserve space for a single-digit decimal (one place) to avoid tearing
  // lv_obj_set_width(val_label_dec, 64); // fixed width for ".X" (wider to avoid wrapping)
  // Prevent LVGL from breaking the label into multiple lines; clip overflow instead
  // lv_label_set_long_mode(val_label_dec, LV_LABEL_LONG_CLIP);
  // Ensure label height can hold the large numeric font to avoid vertical clipping/wrapping
  // lv_obj_set_height(val_label_dec, 140);
  // lv_obj_set_style_text_align(val_label_dec, LV_TEXT_ALIGN_CENTER, 0);

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

void load_current_style() {
    lv_obj_clean(gauge_scr);
    lv_obj_set_style_bg_color(gauge_scr, lv_color_hex(color_background), 0);

    //lv_obj_t * img = lv_image_create(gauge_scr);
    //lv_image_set_src(img, &gauge_bg);
    //lv_obj_center(img);
    //lv_obj_set_style_image_opa(img, 50, 0);

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
    
    // COMMENTED OUT: Horizontal bar UI
    // bar = lv_bar_create(lv_scr_act());
    // lv_obj_set_size(bar, 380, 48); // twice as thick
    // lv_obj_align(bar, LV_ALIGN_CENTER, 0, 40); // move bar up to avoid network icon overlap
    // lv_bar_set_range(bar, 0, 100);
    // lv_bar_set_value(bar, 0, LV_ANIM_OFF);
    // lv_obj_set_style_bg_color(bar, lv_color_hex(color_bar_bg), LV_PART_MAIN);
    // lv_obj_set_style_bg_opa(bar, 255, LV_PART_MAIN);
    // lv_obj_set_style_bg_color(bar, lv_color_make(0,0,0), LV_PART_INDICATOR);
    // lv_obj_set_style_bg_opa(bar, 255, LV_PART_INDICATOR);
    // lv_obj_set_style_pad_all(bar, 4, 0);
    // lv_obj_set_style_radius(bar, 4, LV_PART_MAIN);
    // lv_obj_set_style_radius(bar, 4, LV_PART_INDICATOR);
    // lv_obj_remove_style(bar, NULL, LV_PART_KNOB);
    // lv_obj_set_style_clip_corner(bar, true, LV_PART_MAIN);
    // lv_obj_set_style_clip_corner(bar, true, LV_PART_INDICATOR);
    

    // COMMENTED OUT: Peak hold stripe
    // peak_dot = lv_obj_create(lv_scr_act());
    // lv_obj_set_size(peak_dot, 4, 56); // thin vertical stripe that extends above/below bar
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
      // Force LVGL to remeasure before reading width — avoids stale-cache lag on variable-width fonts
      lv_obj_update_layout(val_label_int);
      lv_obj_update_layout(val_label_dec);
      int int_w = lv_obj_get_width(val_label_int);
      int dec_w = lv_obj_get_width(val_label_dec);
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
    esp_err_t err = twai_receive(&message, pdMS_TO_TICKS(5));
    if (err == ESP_OK) {
      xQueueSend(canMsgQueue, &message, 0);
    } else if (err != ESP_ERR_TIMEOUT) {
      // Only attempt recovery on actual bus errors, rate-limited to once per 5s
      unsigned long now = millis();
      if (now - last_recover_ms > 5000) {
        last_recover_ms = now;
        canbus_recover();
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
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
  current_brightness = preferences.getInt("bright", 40);
  peak_hold_enabled = preferences.getBool("peak", true);
  debug_mode_enabled = preferences.getBool("dbg", false);
  current_font = (uint8_t)preferences.getUInt("font", 0);
  device_name = preferences.getString("devname", defaultName);
  secondary_metric = (uint8_t)preferences.getUInt("sm", 0);
  current_page = (DisplayPage)preferences.getUInt("page", 0);
  preferences.end();

  gauge_scr = lv_scr_act();   // the default screen holds the gauge UI
  load_current_style();
  build_glowcraft_page();
  apply_page();               // load whichever page was last selected
  setup_wifi();

  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);

  // Render a few frames before enabling backlight — ensures the framebuffer
  // contains valid content before it's visible, preventing startup corruption
  for (int i = 0; i < 5; i++) lv_timer_handler();
  set_backlight(current_brightness);
}

void loop() {
  unsigned long lvgl_t = millis();
  lv_timer_handler();
  if (show_perf_stats) perf_lvgl_ms = millis() - lvgl_t;
  server.handleClient();
  ArduinoOTA.handle();
  
  // --- FLAG HANDLERS ---
  if (flag_reboot) { delay(500); ESP.restart(); }
  if (flag_theme_update) {
      flag_theme_update = false;
      load_current_style(); 
  }
  if (flag_bright_update) {
      flag_bright_update = false;
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