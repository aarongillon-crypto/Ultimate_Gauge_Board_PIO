#include <Arduino.h>
#include "CANBus_Driver.h"
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
#include <math.h>

// --- CONFIGURATION ---
bool test_mode_enabled = false; 
bool show_perf_stats = false; 
bool peak_hold_enabled = true; // New Toggle

enum GaugeMode { MODE_BOOST=0, MODE_AFR=1, MODE_WATER=2, MODE_OIL=3 };

LV_FONT_DECLARE(dseg14_60);
LV_FONT_DECLARE(dseg14_96);
LV_FONT_DECLARE(dseg14_120)
// LV_IMG_DECLARE(gauge_bg);

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

typedef struct {
  float boost_psi; float afr_gas; int rpm; int water_temp_c; float oil_press_psi;
} HaltechData_t;
HaltechData_t HaltechData;

Preferences preferences;
WebServer server(80);
GaugeMode current_mode = MODE_BOOST; 

uint32_t text_color = 0xFFD700;
uint32_t color_low = 0x2196F3, color_mid = 0x4CAF50, color_high = 0xF44336;
uint32_t color_mode_label = 0x969696; // Mode label (gray)
uint32_t color_link_icon = 0x00C851; // Connectivity icon (green)
uint32_t needle_color = 0xFF6600;     // Needle color (orange)
uint32_t color_peak = 0xFFFFFF;      // Peak stripe (white)
uint32_t color_background = 0x000000; // Screen background (black)
uint32_t current_applied_text = 0;
int current_brightness = 40;
// Forward declarations

float displayed_val = 0.0; 
float target_val = 0.0;
float peak_val = -999.0;
unsigned long peak_timer = 0;
const unsigned long PEAK_HOLD_TIME = 30000;
unsigned long last_data_time = 0;
unsigned long last_broadcast = 0;

// PERF STATS
unsigned long perf_last_time = 0;
int perf_frames = 0;
int perf_fps = 0;
int perf_frame_ms = 0;

volatile bool flag_new_peer = false;
volatile bool flag_reboot = false;
volatile bool flag_theme_update = false; 
volatile bool flag_bright_update = false;
volatile bool flag_stats_update = false;

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
lv_obj_t *perf_label;
lv_obj_t *needle_tip; 

const float RANGES[4][2] = { {-15,30}, {8,22}, {0,120}, {0,100} };
const char* MODE_NAMES[4] = { "BOOST", "AFR", "WATER", "OIL P" };

bool receiving_data = false;
volatile bool data_ready = false;

void drivers_init() {
  i2c_init(); tca9554pwr_init(0x00); lcd_init(); canbus_init(); lvgl_init();
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
  EspNowPacket *pkt = (EspNowPacket *)incomingData;

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
  peerInfo.channel = WIFI_CHANNEL;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_AP; 
  if (!esp_now_is_peer_exist(broadcastAddress)) esp_now_add_peer(&peerInfo);
  esp_now_send(broadcastAddress, (uint8_t *) pkt, sizeof(EspNowPacket));
}

void broadcast_presence() {
  EspNowPacket pkt; pkt.type = 1; pkt.mode = current_mode;
  broadcast_packet(&pkt);
}

void send_remote_command(uint8_t *targetMac, int newMode) {
  EspNowPacket pkt; pkt.type = 2; pkt.mode = newMode;
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, targetMac, 6);
  peerInfo.channel = WIFI_CHANNEL;
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
  String html = "<html><head><meta name='viewport' content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;padding:10px;background:#222;color:#fff;} .card{background:#333;margin:10px;padding:15px;border-radius:10px;} button{font-size:16px;width:45%;padding:10px;margin:5px;border:none;border-radius:5px;cursor:pointer;} .btn-b{background:#0099ff;color:white;} .btn-a{background:#00cc66;color:white;} .btn-w{background:#ff9900;color:white;} .btn-o{background:#cc3300;color:white;} input[type=color]{width:50px;height:40px;border:none;vertical-align:middle;margin:5px;} label{display:inline-block;width:60px;text-align:right;} input[type=range]{width:60%;vertical-align:middle;}</style></head><body>";
  html += "<h1>Fleet Config</h1>";
  html += "<p>Peers Found: " + String(fleet_count) + "</p>";
  
  html += "<div class='card'><h3>DYNAMIC ELEMENTS</h3><form action='/theme' method='get'><div><label>Text:</label><input type='color' name='ct' value='" + colorToHex(text_color) + "'></div><div><label>Low:</label><input type='color' name='cl' value='" + colorToHex(color_low) + "'></div><div><label>Mid:</label><input type='color' name='cm' value='" + colorToHex(color_mid) + "'></div><div><label>High:</label><input type='color' name='ch' value='" + colorToHex(color_high) + "'></div><button style='width:auto;margin-top:10px;background:#d32f2f;color:white;'>Apply to ALL</button></form></div>";



  html += "<div class='card'><h3>STATIC ELEMENTS</h3><form action='/uicolors' method='get'><div><label>Background:</label><input type='color' name='cbg' value='" + colorToHex(color_background) + "'></div><div><label>Mode Label:</label><input type='color' name='cml' value='" + colorToHex(color_mode_label) + "'></div><div><label>Link Icon:</label><input type='color' name='cli' value='" + colorToHex(color_link_icon) + "'></div><div><label>Needle:</label><input type='color' name='cn' value='" + colorToHex(needle_color) + "'></div><div><label>Peak Stripe:</label><input type='color' name='cp' value='" + colorToHex(color_peak) + "'></div><button style='width:auto;margin-top:10px;background:#2196F3;color:white;'>Apply to ALL</button></form></div>";

  html += "<div class='card'><h3>GLOBAL CONTROLS</h3><form action='/bright' method='get'><label>Brightness: </label><input type='range' name='b' min='10' max='100' value='" + String(current_brightness) + "' onchange='this.form.submit()'></form>";
  html += "<a href='/test?t=" + String(!test_mode_enabled) + "'><button class='btn'>Test Mode: " + String(test_mode_enabled?"ON":"OFF") + "</button></a>";
  html += "<br><a href='/stats?s=" + String(!show_perf_stats) + "'><button class='btn'>Stats: " + String(show_perf_stats?"ON":"OFF") + "</button></a>";
  html += "</div>";
  
  html += "<div class='card'><h3>LOCAL GAUGE</h3>";
  // PEAK TOGGLE
  html += "<a href='/peak?p=" + String(!peak_hold_enabled) + "'><button class='btn'>Peak Hold: " + String(peak_hold_enabled?"ON":"OFF") + "</button></a><br>";
  
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
        EspNowPacket pkt; pkt.type = 3; pkt.c1=text_color; pkt.c2=color_low; pkt.c3=color_mid; pkt.c4=color_high;
        broadcast_packet(&pkt);
        flag_theme_update = true; 
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handleSet() {
    if (server.hasArg("mode")) {
        int m = server.arg("mode").toInt();
        preferences.begin("gauge", false); preferences.putInt("mode", m); preferences.end();
        ESP.restart();
    }
}
void handleTest() {
    if (server.hasArg("t")) test_mode_enabled = server.arg("t").toInt();
    EspNowPacket pkt; pkt.type = 4; pkt.value = test_mode_enabled?1:0; broadcast_packet(&pkt);
    server.sendHeader("Location", "/"); server.send(303);
}
void handleStats() {
    if (server.hasArg("s")) show_perf_stats = server.arg("s").toInt();
    EspNowPacket pkt; pkt.type = 6; pkt.value = show_perf_stats?1:0; broadcast_packet(&pkt);
    flag_stats_update = true;
    server.sendHeader("Location", "/"); server.send(303);
}
void handleBright() {
    if (server.hasArg("b")) {
        int b = server.arg("b").toInt();
        current_brightness = b; set_backlight(b);
        preferences.begin("gauge", false); preferences.putInt("bright", b); preferences.end();
        EspNowPacket pkt; pkt.type = 5; pkt.value = b; broadcast_packet(&pkt);
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handlePeak() {
    if (server.hasArg("p")) {
        peak_hold_enabled = server.arg("p").toInt();
        preferences.begin("gauge", false); preferences.putBool("peak", peak_hold_enabled); preferences.end();
        server.sendHeader("Location", "/"); server.send(303);
    }
}
void handleRemote() {
    if (server.hasArg("mac") && server.hasArg("mode")) {
      String macStr = server.arg("mac");
        int m = server.arg("mode").toInt();
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
        EspNowPacket pkt; 
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

  uint8_t mac[6]; WiFi.macAddress(mac);
  char ssid[32]; snprintf(ssid, sizeof(ssid), "Haltech-Gauge-%02X%02X", mac[4], mac[5]);
  WiFi.softAP(ssid, NULL, WIFI_CHANNEL);
  
  if (esp_now_init() != ESP_OK) return;
  esp_now_register_recv_cb(OnDataRecv);
  
  server.on("/", handleRoot);
  server.on("/theme", handleTheme); server.on("/set", handleSet); server.on("/rem", handleRemote);
  server.on("/bright", handleBright); server.on("/test", handleTest); server.on("/stats", handleStats);
  server.on("/peak", handlePeak); server.on("/uicolors", handleUIColors);
  server.begin();
}

// --- UI ---
void common_label_setup() {
  val_label_int = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_int, true, 0);

  val_label_dec = lv_label_create(lv_scr_act());
  lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_dec, true, 0); 

  // Reserve space for a single-digit decimal (one place) to avoid tearing
  // lv_obj_set_width(val_label_dec, 64); // fixed width for ".X" (wider to avoid wrapping)
  // Prevent LVGL from breaking the label into multiple lines; clip overflow instead
  // lv_label_set_long_mode(val_label_dec, LV_LABEL_LONG_CLIP);
  // Ensure label height can hold the large numeric font to avoid vertical clipping/wrapping
  // lv_obj_set_height(val_label_dec, 140);
  // lv_obj_set_style_text_align(val_label_dec, LV_TEXT_ALIGN_CENTER, 0);

    mode_label = lv_label_create(lv_scr_act());
    #ifdef LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_28, 0);
    #else
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_set_style_text_color(mode_label, lv_color_hex(color_mode_label), 0);
    lv_label_set_text(mode_label, MODE_NAMES[current_mode]);
    // Use native 96px dseg font (no transform needed)
    lv_obj_set_style_text_font(val_label_int, &dseg14_120, 0);
    lv_obj_set_style_text_font(val_label_dec, &dseg14_96, 0);
}

void load_current_style() {
    lv_obj_clean(lv_scr_act());
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(color_background), 0);
    
    //lv_obj_t * img = lv_image_create(lv_scr_act());
    //lv_image_set_src(img, &gauge_bg);
    //lv_obj_center(img);
    //lv_obj_set_style_image_opa(img, 50, 0);

    // LINK ICON
    link_icon = lv_label_create(lv_scr_act());
    lv_obj_set_style_text_font(link_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(link_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(link_icon, lv_color_hex(color_link_icon), 0);
    lv_obj_align(link_icon, LV_ALIGN_BOTTOM_MID, 0, -80); 
    if(fleet_count == 0) lv_obj_add_flag(link_icon, LV_OBJ_FLAG_HIDDEN);

    // PERF OVERLAY (MOVED TO CENTER-TOP)
    perf_label = lv_label_create(lv_scr_act());
    lv_obj_align(perf_label, LV_ALIGN_CENTER, 0, -140); // Move performance monitor above main text
    lv_obj_set_style_text_color(perf_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(perf_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(perf_label, 150, 0); 
    if(!show_perf_stats) lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);

    // === STATIC RING INDICATOR (outer border, color-changing) ===
    bar = lv_obj_create(lv_scr_act());
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
    peak_dot = lv_obj_create(lv_scr_act());
    lv_obj_set_size(peak_dot, 8, 8);  // Small indicator dot (hidden for now)
    lv_obj_set_style_radius(peak_dot, 4, 0);
    lv_obj_set_style_bg_color(peak_dot, lv_color_hex(color_peak), 0);
    lv_obj_set_style_border_width(peak_dot, 0, 0);
    lv_obj_set_pos(peak_dot, 0, 0);
    if(!peak_hold_enabled) lv_obj_add_flag(peak_dot, LV_OBJ_FLAG_HIDDEN); // Initial State
    
    // NEEDLE TIP - thin triangle that orbits around center
    needle_tip = lv_line_create(lv_scr_act());
    lv_obj_set_style_line_width(needle_tip, 8, 0);  // Thin line width
    lv_obj_set_style_line_color(needle_tip, lv_color_hex(needle_color), 0);
    lv_obj_set_style_line_rounded(needle_tip, 0, 0);  // Not rounded
    
    common_label_setup();
    lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -40);
    lv_obj_align(val_label_int, LV_ALIGN_CENTER, 50, -10); // Move right 40 px, up 10 px
    lv_obj_align(val_label_dec, LV_ALIGN_CENTER, 50, -10);  // Move right 40 px, up 10 px
    
    current_applied_text = 0; 
}

void update_ui(float val, float min, float max, float peak, uint32_t color_hex) {
    // Ring indicator: only update color based on gauge value
    static uint32_t prev_color = 0;
    if (color_hex != prev_color) {
        lv_obj_set_style_border_color(bar, lv_color_hex(color_hex), 0);
        prev_color = color_hex;
    }

    // Peak indicator (optional, currently not used)
    if (peak_hold_enabled) {
      lv_obj_clear_flag(peak_dot, LV_OBJ_FLAG_HIDDEN);
    } else {
      lv_obj_add_flag(peak_dot, LV_OBJ_FLAG_HIDDEN);
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
    
    // Line points: start, tip
    static lv_point_precise_t needle_points[2];
    
    // Start point at 225px radius
    needle_points[0].x = center_x + (int)(radius_start * cosf(angle_rad));
    needle_points[0].y = center_y + (int)(radius_start * sinf(angle_rad));
    
    // Tip point at 240px radius
    needle_points[1].x = center_x + (int)(radius_end * cosf(angle_rad));
    needle_points[1].y = center_y + (int)(radius_end * sinf(angle_rad));
    
    lv_line_set_points(needle_tip, needle_points, 2);
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
        if (millis() - peak_timer > PEAK_HOLD_TIME) peak_val = target_val;
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
    if (strcmp(prev_b1, b1) != 0) {
      lv_label_set_text(val_label_int, b1);
      strncpy(prev_b1, b1, sizeof(prev_b1));
    }
    if (strcmp(prev_b2, b2) != 0) {
      lv_label_set_text(val_label_dec, b2);
      strncpy(prev_b2, b2, sizeof(prev_b2));
    }
    // Dynamic spacing: right-justify integer label and place decimal to its right
    static int prev_int_w = 0; static int prev_dec_w = 0;
    int int_w = lv_obj_get_width(val_label_int);
    int dec_w = lv_obj_get_width(val_label_dec);
    if (int_w != prev_int_w || dec_w != prev_dec_w) {
      prev_int_w = int_w; prev_dec_w = dec_w;
      int spacing = 12;  // Gap between integer and decimal
      // Anchor the integer's RIGHT edge at this x (relative to center)
      int anchor_x = -(spacing / 2);
      int int_center_x = anchor_x - (int_w / 2) + 50;  // +40 px right
      int dec_center_x = anchor_x + spacing + (dec_w / 2) + 50;  // +40 px right
      lv_obj_align(val_label_int, LV_ALIGN_CENTER, int_center_x, 5);  // Y: 10 (up 10 px from 20)
      lv_obj_align(val_label_dec, LV_ALIGN_CENTER, dec_center_x, 5);   // Y: 10 (up 10 px from 20)
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
          HaltechData.rpm = get_uint16_be(message.data, 0);
          uint16_t raw_map = get_uint16_be(message.data, 2);
          HaltechData.boost_psi = (raw_map * 0.1 - 101.3) * 0.145038;
          break;
        }
        case 0x361: { 
          uint16_t raw_oil = get_uint16_be(message.data, 2);
          HaltechData.oil_press_psi = (raw_oil * 0.1) * 0.145038; 
          break;
        }
        case 0x362: { 
          uint16_t raw_coolant = get_uint16_be(message.data, 0);
          HaltechData.water_temp_c = (raw_coolant / 10) - 273;
          break;
        }
        case 0x368: { 
          uint16_t raw_lambda = get_uint16_be(message.data, 0);
          HaltechData.afr_gas = (raw_lambda / 1000.0) * 14.7;
          break;
        }
      }
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void receive_can_task(void *arg) {
  while (1) {
    twai_message_t message;
    if (twai_receive(&message, pdMS_TO_TICKS(5)) == ESP_OK) {
      xQueueSend(canMsgQueue, &message, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup() {
  Serial.begin(115200);
  drivers_init();
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0); 

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
  peak_hold_enabled = preferences.getBool("peak", true); // LOAD PEAK SETTING
  preferences.end();

  set_backlight(current_brightness);
  load_current_style(); 

  setup_wifi();
  
  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);
}

void loop() {
  lv_timer_handler();
  server.handleClient();
  
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

  // --- STATS LOGIC ---
  if (show_perf_stats) {
      perf_frames++;
      if (millis() - perf_last_time >= 1000) {
          perf_fps = perf_frames;
          perf_frames = 0;
          perf_last_time = millis();
          lv_label_set_text_fmt(perf_label, "FPS: %d\nMS: %d", perf_fps, perf_frame_ms);
      }
  }

  if (millis() - last_broadcast > 2000) {
      last_broadcast = millis();
      broadcast_presence();
  }
  
  if (millis() - last_data_time > 33) { 
      unsigned long start = millis();
      last_data_time = start;
      if (test_mode_enabled) {
          static float t=0; t+=0.05;
          HaltechData.boost_psi = -15 + (sin(t) + 1) * 22.5; 
          HaltechData.afr_gas = 8 + (sin(t*0.5) + 1) * 7.0; 
          HaltechData.water_temp_c = 50 + (sin(t*0.3) + 1) * 35.0; 
          HaltechData.oil_press_psi = 10 + (sin(t*0.7) + 1) * 45.0; 
      }
      update_gauge_master();
      
      if(show_perf_stats) perf_frame_ms = millis() - start;
  }
  yield();
  vTaskDelay(pdMS_TO_TICKS(5));
}