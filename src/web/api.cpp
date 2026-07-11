#include "api.h"
#include "../app_state.h"
#include "../config_store.h"
#include "../themes.h"
#include "../fleet.h"
#include "../haltech_decode.h"
#include "CANBus_Driver.h"
#include "Display_ST7701.h"
#include <ArduinoJson.h>
#include <uri/UriBraces.h>

static WebServer* srv = nullptr;

static String hexStr(uint32_t c) {
  char buf[8]; snprintf(buf, sizeof(buf), "#%06X", c); return String(buf);
}
static uint32_t parseHex(const char* s) {
  if (!s) return 0;
  if (*s == '#') s++;
  return strtoul(s, NULL, 16);
}

// ---------- /api/state ----------
static void apiState() {
  JsonDocument doc;
  doc["name"] = device_name;
  doc["fw"] = FIRMWARE_VERSION;
  doc["build"] = FIRMWARE_BUILD;
  doc["mode"] = (int)current_mode;
  doc["modeLabel"] = behavior.mode[current_mode].label;
  doc["page"] = (int)current_page;
  doc["bright"] = current_brightness;
  doc["font"] = current_font;
  doc["test"] = test_mode_enabled;
  doc["stats"] = show_perf_stats;
  doc["dbg"] = debug_mode_enabled;
  doc["peak"] = peak_hold_enabled;
  doc["sec"] = secondary_chan;
  doc["slot"] = active_theme;
  doc["tpsync"] = trimpot_theme_sync;
  doc["peers"] = fleet_count;
  doc["canOk"] = canbus_ok;
  doc["uptime"] = millis() / 1000;
  doc["heap"] = ESP.getFreeHeap();
  // Live values for the 4 configured modes + secondary (display units).
  JsonArray live = doc["live"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    int ci = chan_index_from_key(behavior.mode[i].chan_key);
    live.add(chan_display(ci, haltech_value(ci)));
  }
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

// ---------- /api/channels — the machine-readable registry contract ----------
// Metadata + live values for every channel. This is the same contract the
// planned PC/Web layout-designer GUI consumes. key=0 marks bit-addressed
// channels (not bindable as gauge modes).
static void apiChannels() {
  JsonDocument doc;
  JsonArray arr = doc["channels"].to<JsonArray>();
  for (int i = 0; i < HALTECH_CHANNEL_COUNT; i++) {
    const HaltechChannel& c = HALTECH_CHANNELS[i];
    JsonObject o = arr.add<JsonObject>();
    char idbuf[8]; snprintf(idbuf, sizeof(idbuf), "0x%03X", c.can_id);
    o["key"] = chan_key(i);
    o["id"] = idbuf;
    o["off"] = c.offset;
    if (c.bit_start != 0xFF) o["bit"] = c.bit_start;
    o["name"] = c.name;
    o["unit"] = chan_unit_str(i);
    uint32_t age = haltech_age_ms(i);
    if (age != UINT32_MAX) {
      o["val"] = serialized(String(chan_display(i, haltech_value(i)), 2));
      o["age"] = age;
    }
  }
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

// ---------- themes ----------
static void themeToJson(uint8_t i, JsonObject o) {
  const GaugeTheme& t = themes[i];
  o["fmt"] = "ugb-theme"; o["v"] = 1;
  o["name"] = theme_names[i];
  JsonObject c = o["colors"].to<JsonObject>();
  c["text"] = hexStr(t.text); c["low"] = hexStr(t.low);
  c["mid"] = hexStr(t.mid);   c["high"] = hexStr(t.high);
  c["bg"] = hexStr(t.background); c["modeLabel"] = hexStr(t.mode_label);
  c["linkIcon"] = hexStr(t.link_icon); c["needle"] = hexStr(t.needle);
  c["peak"] = hexStr(t.peak);
  JsonObject g = o["gradient"].to<JsonObject>();
  g["type"] = t.bg_grad_type; g["stops"] = t.bg_grad_stops;
  g["angle"] = t.bg_grad_angle;
  g["c2"] = hexStr(t.bg_grad2); g["c3"] = hexStr(t.bg_grad3);
}

static void apiThemesList() {
  JsonDocument doc;
  doc["active"] = active_theme;
  JsonArray arr = doc["slots"].to<JsonArray>();
  for (uint8_t i = 0; i < THEME_SLOTS; i++) themeToJson(i, arr.add<JsonObject>());
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

static void apiThemeGet() {
  int i = srv->pathArg(0).toInt();
  if (i < 0 || i >= THEME_SLOTS) { srv->send(404, "text/plain", "No such slot"); return; }
  JsonDocument doc;
  themeToJson((uint8_t)i, doc.to<JsonObject>());
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

// Broadcast the full active theme to the fleet via the v1 packet triple
// (types 3 + 7 + 8) — same on-wire behavior as the old per-card "Apply to ALL".
static void broadcastActiveTheme() {
  EspNowPacket p = {};
  p.type = 3; p.c1 = text_color; p.c2 = color_low; p.c3 = color_mid; p.c4 = color_high;
  broadcast_packet(&p);
  p = {}; p.type = 7; p.c1 = color_background; p.c2 = color_mode_label;
  p.c3 = color_link_icon; p.c4 = needle_color; p.value = (int)color_peak;
  broadcast_packet(&p);
  p = {}; p.type = 8; p.c1 = color_background2; p.c2 = color_background3; p.c3 = color_background;
  p.value = (int)((uint32_t)bg_grad_type | ((uint32_t)bg_grad_stops << 4) | ((uint32_t)bg_grad_angle << 8));
  broadcast_packet(&p);
}

// POST /api/themes/{slot} — body is the theme JSON (doubles as import).
// Persists the slot; if it's the active slot, applies live + pushes to fleet.
static void apiThemeSet() {
  int i = srv->pathArg(0).toInt();
  if (i < 0 || i >= THEME_SLOTS) { srv->send(404, "text/plain", "No such slot"); return; }
  String body = srv->arg("plain");
  if (body.length() == 0 || body.length() > 4096) { srv->send(400, "text/plain", "Bad body"); return; }

  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) { srv->send(400, "text/plain", "Bad JSON"); return; }
  if (strcmp(doc["fmt"] | "", "ugb-theme") != 0 || (int)(doc["v"] | 0) != 1) {
    srv->send(400, "text/plain", "Not a ugb-theme v1"); return;
  }
  JsonObject c = doc["colors"];
  JsonObject g = doc["gradient"];
  if (c.isNull() || g.isNull()) { srv->send(400, "text/plain", "Missing colors/gradient"); return; }

  GaugeTheme t;
  t.text = parseHex(c["text"]); t.low = parseHex(c["low"]);
  t.mid = parseHex(c["mid"]);   t.high = parseHex(c["high"]);
  t.background = parseHex(c["bg"]); t.mode_label = parseHex(c["modeLabel"]);
  t.link_icon = parseHex(c["linkIcon"]); t.needle = parseHex(c["needle"]);
  t.peak = parseHex(c["peak"]);
  t.bg_grad2 = parseHex(g["c2"]); t.bg_grad3 = parseHex(g["c3"]);
  t.bg_grad_type  = (uint8_t)constrain((int)(g["type"] | 0), 0, 5);
  t.bg_grad_stops = ((int)(g["stops"] | 2) == 3) ? 3 : 2;
  t.bg_grad_angle = (uint16_t)constrain((int)(g["angle"] | 0), 0, 360);

  String name = String((const char*)(doc["name"] | ""));
  name.trim();
  if (name.length() == 0) name = "P" + String(i);
  if (name.length() > 20) name = name.substring(0, 20);

  themes[i] = t;
  theme_names[i] = name;
  persist_theme((uint8_t)i);
  cfg_put_theme_name((uint8_t)i, name);

  if ((uint8_t)i == active_theme) {
    theme_to_globals(active_theme);
    // Keep the legacy live keys in sync (slot-0 migration path reads them).
    cfg_put_uint("ct", text_color); cfg_put_uint("cl", color_low);
    cfg_put_uint("cm", color_mid);  cfg_put_uint("ch", color_high);
    cfg_put_uint("cbg", color_background); cfg_put_uint("cml", color_mode_label);
    cfg_put_uint("cli", color_link_icon);  cfg_put_uint("cn", needle_color);
    cfg_put_uint("cp", color_peak);
    cfg_put_uint("cbg2", color_background2); cfg_put_uint("cbg3", color_background3);
    cfg_put_uchar("cgt", bg_grad_type); cfg_put_uchar("cgs", bg_grad_stops);
    cfg_put_ushort("cga", bg_grad_angle);
    flag_theme_update = true;
    broadcastActiveTheme();   // active-slot edits behave like the old "Apply to ALL"
  }
  srv->send(200, "text/plain", "OK");
}

static void apiThemeActivate() {
  int i = srv->pathArg(0).toInt();
  if (i < 0 || i >= THEME_SLOTS) { srv->send(404, "text/plain", "No such slot"); return; }
  active_theme = (uint8_t)i;
  cfg_put_uint("atheme", active_theme);
  theme_to_globals(active_theme);
  flag_theme_update = true;
  srv->send(200, "text/plain", String((int)active_theme));
}

static void apiThemeCopy() {
  int dst = srv->pathArg(0).toInt();
  int src = srv->hasArg("from") ? srv->arg("from").toInt() : -1;
  if (dst < 0 || dst >= THEME_SLOTS || src < 0 || src >= THEME_SLOTS || src == dst) {
    srv->send(400, "text/plain", "Bad slots"); return;
  }
  themes[dst] = themes[src];
  theme_names[dst] = theme_names[src];
  persist_theme((uint8_t)dst);
  cfg_put_theme_name((uint8_t)dst, theme_names[dst]);
  if ((uint8_t)dst == active_theme) { theme_to_globals(active_theme); flag_theme_update = true; }
  srv->send(200, "text/plain", "OK");
}

// ---------- behavior config ----------
static void apiConfigGet() {
  JsonDocument doc;
  JsonArray modes = doc["modes"].to<JsonArray>();
  for (int i = 0; i < 4; i++) {
    JsonObject m = modes.add<JsonObject>();
    m["chan"] = behavior.mode[i].chan_key;
    m["label"] = behavior.mode[i].label;
    m["min"] = behavior.mode[i].min; m["max"] = behavior.mode[i].max;
    m["z1"] = behavior.mode[i].z1;   m["z2"] = behavior.mode[i].z2;
  }
  doc["smoothing"] = behavior.smoothing;
  doc["maxRate"] = behavior.max_rate;
  doc["peakHoldMs"] = behavior.peak_hold_ms;
  JsonObject u = doc["units"].to<JsonObject>();
  u["psi"] = units_press_psi; u["degF"] = units_temp_f;
  u["mph"] = units_speed_mph; u["afr"] = units_lambda_afr;
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

// POST /api/config — apply + persist + broadcast CONFIG_SYNC to the fleet.
// Validation: min<max per mode only; z1/z2 outside the range is legitimate
// (that's how always-one-colour modes like WATER/OIL are expressed).
static void apiConfigSet() {
  String body = srv->arg("plain");
  if (body.length() == 0 || body.length() > 4096) { srv->send(400, "text/plain", "Bad body"); return; }
  JsonDocument doc;
  if (deserializeJson(doc, body) != DeserializationError::Ok) { srv->send(400, "text/plain", "Bad JSON"); return; }
  JsonArray modes = doc["modes"];
  if (modes.isNull() || modes.size() != 4) { srv->send(400, "text/plain", "Need 4 modes"); return; }

  BehaviorConfig b = behavior;
  for (int i = 0; i < 4; i++) {
    JsonObject m = modes[i];
    uint16_t ck = m["chan"] | behavior.mode[i].chan_key;
    if (chan_index_from_key(ck) < 0) { srv->send(400, "text/plain", "Unknown channel"); return; }
    b.mode[i].chan_key = ck;
    const char* lbl = m["label"] | behavior.mode[i].label;
    strncpy(b.mode[i].label, lbl, sizeof(b.mode[i].label) - 1);
    b.mode[i].label[sizeof(b.mode[i].label) - 1] = 0;
    if (!b.mode[i].label[0]) strncpy(b.mode[i].label, "GAUGE", sizeof(b.mode[i].label));
    b.mode[i].min = m["min"] | behavior.mode[i].min;
    b.mode[i].max = m["max"] | behavior.mode[i].max;
    b.mode[i].z1  = m["z1"]  | behavior.mode[i].z1;
    b.mode[i].z2  = m["z2"]  | behavior.mode[i].z2;
    if (!(b.mode[i].min < b.mode[i].max)) { srv->send(400, "text/plain", "min must be < max"); return; }
  }
  b.smoothing    = constrain((float)(doc["smoothing"] | behavior.smoothing), 0.02f, 1.0f);
  b.max_rate     = constrain((float)(doc["maxRate"] | behavior.max_rate), 1.0f, 10000.0f);
  b.peak_hold_ms = constrain((uint32_t)(doc["peakHoldMs"] | behavior.peak_hold_ms), (uint32_t)1000, (uint32_t)600000);

  JsonObject u = doc["units"];
  if (!u.isNull()) {
    units_press_psi  = u["psi"]  | units_press_psi;   cfg_put_bool("u_psi",  units_press_psi);
    units_temp_f     = u["degF"] | units_temp_f;      cfg_put_bool("u_degf", units_temp_f);
    units_speed_mph  = u["mph"]  | units_speed_mph;   cfg_put_bool("u_mph",  units_speed_mph);
    units_lambda_afr = u["afr"]  | units_lambda_afr;  cfg_put_bool("u_afr",  units_lambda_afr);
  }

  behavior = b;
  cfg_persist_behavior(behavior);
  flag_mode_update = true;      // refresh gauge label + snap to the (possibly new) channel
  fleet_push_config(nullptr);   // broadcast to all peers
  srv->send(200, "text/plain", "OK");
}

// ---------- fleet ----------
static void apiFleet() {
  PeerGauge peers[10];
  int n = fleet_snapshot(peers, 10);
  JsonDocument doc;
  JsonArray arr = doc["peers"].to<JsonArray>();
  unsigned long now = millis();
  for (int i = 0; i < n; i++) {
    if (now - peers[i].last_seen >= 10000) continue;  // aged out
    JsonObject o = arr.add<JsonObject>();
    char mac[13];
    snprintf(mac, sizeof(mac), "%02X%02X%02X%02X%02X%02X",
             peers[i].mac[0], peers[i].mac[1], peers[i].mac[2],
             peers[i].mac[3], peers[i].mac[4], peers[i].mac[5]);
    o["mac"] = mac;
    o["mode"] = constrain(peers[i].mode, 0, 3);
    o["age"] = now - peers[i].last_seen;
    o["proto"] = peers[i].proto;
    if (peers[i].proto >= 2) {
      o["name"] = peers[i].name;
      char fw[12]; snprintf(fw, sizeof(fw), "%u.%u.%u", peers[i].fw[0], peers[i].fw[1], peers[i].fw[2]);
      o["fw"] = fw;
      o["slot"] = peers[i].active_theme;
    }
  }
  String out; serializeJson(doc, out);
  srv->send(200, "application/json", out);
}

// Parse the {mac} path arg: 12 hex chars → mac bytes, or "ALL" → broadcast
// (returns false for broadcast, true for unicast; bad input sends 400 + throws off).
static bool parseMacArg(uint8_t mac[6], bool* isAll) {
  String macStr = srv->pathArg(0);
  if (macStr == "ALL") { *isAll = true; return true; }
  *isAll = false;
  if (macStr.length() != 12) { srv->send(400, "text/plain", "Bad MAC"); return false; }
  for (int i = 0; i < 6; i++) mac[i] = (uint8_t)strtol(macStr.substring(i*2, i*2+2).c_str(), NULL, 16);
  return true;
}

static void apiFleetMode() {
  uint8_t mac[6]; bool all;
  if (!parseMacArg(mac, &all)) return;
  if (all || !srv->hasArg("v")) { srv->send(400, "text/plain", "Bad Request"); return; }
  int m = constrain(srv->arg("v").toInt(), 0, 3);
  send_remote_command(mac, m);
  srv->send(200, "text/plain", "OK");
}

// POST /api/fleet/{mac}/theme?slot=N&activate=0|1 — push a local slot to one
// peer (or all with mac=ALL). Legacy peers get the v1 fallback automatically.
static void apiFleetTheme() {
  uint8_t mac[6]; bool all;
  if (!parseMacArg(mac, &all)) return;
  int slot = srv->hasArg("slot") ? srv->arg("slot").toInt() : -1;
  if (slot < 0 || slot >= THEME_SLOTS) { srv->send(400, "text/plain", "Bad slot"); return; }
  bool activate = srv->arg("activate") == "1";
  fleet_push_theme(all ? nullptr : mac, (uint8_t)slot, activate);
  srv->send(200, "text/plain", "OK");
}

static void apiFleetConfig() {
  uint8_t mac[6]; bool all;
  if (!parseMacArg(mac, &all)) return;
  fleet_push_config(all ? nullptr : mac);
  srv->send(200, "text/plain", "OK");
}

static void apiFleetIdentify() {
  uint8_t mac[6]; bool all;
  if (!parseMacArg(mac, &all)) return;
  fleet_send_identify(all ? nullptr : mac);
  srv->send(200, "text/plain", "OK");
}

// ---------- actions ----------
// Small mutations: POST /api/action/{name}?v=... Toggles return the new state
// ("1"/"0") so the SPA confirms rather than guessing — same contract as the
// old toggle handlers.
static void apiAction() {
  String name = srv->pathArg(0);
  String v = srv->arg("v");

  if (name == "mode") {
    int m = constrain(v.toInt(), 0, 3);
    current_mode = (GaugeMode)m;
    cfg_put_int("mode", m);
    flag_mode_update = true;
    srv->send(200, "text/plain", String(m));
  } else if (name == "bright") {
    int b = constrain(v.toInt(), 10, 100);
    current_brightness = b; set_backlight(b);
    cfg_put_int("bright", b);
    EspNowPacket pkt = {}; pkt.type = 5; pkt.value = b; broadcast_packet(&pkt);
    srv->send(200, "text/plain", String(b));
  } else if (name == "test") {
    test_mode_enabled = !test_mode_enabled;
    EspNowPacket pkt = {}; pkt.type = 4; pkt.value = test_mode_enabled?1:0; broadcast_packet(&pkt);
    srv->send(200, "text/plain", test_mode_enabled ? "1" : "0");
  } else if (name == "stats") {
    show_perf_stats = !show_perf_stats;
    EspNowPacket pkt = {}; pkt.type = 6; pkt.value = show_perf_stats?1:0; broadcast_packet(&pkt);
    flag_stats_update = true;
    srv->send(200, "text/plain", show_perf_stats ? "1" : "0");
  } else if (name == "dbg") {
    debug_mode_enabled = !debug_mode_enabled;
    cfg_put_bool("dbg", debug_mode_enabled);
    srv->send(200, "text/plain", debug_mode_enabled ? "1" : "0");
  } else if (name == "peak") {
    peak_hold_enabled = !peak_hold_enabled;
    if (peak_hold_enabled) { peak_val = -999.0f; peak_low_val = 999.0f; }
    cfg_put_bool("peak", peak_hold_enabled);
    srv->send(200, "text/plain", peak_hold_enabled ? "1" : "0");
  } else if (name == "font") {
    current_font = (current_font == 0) ? 1 : 0;
    cfg_put_uint("font", current_font);
    flag_theme_update = true;
    srv->send(200, "text/plain", String((int)current_font));
  } else if (name == "page") {
    int p = constrain(v.toInt(), 0, 1);
    current_page = (DisplayPage)p;
    cfg_put_uint("page", (uint32_t)current_page);
    flag_page_update = true;
    srv->send(200, "text/plain", String(p));
  } else if (name == "sec") {
    uint16_t ck = (uint16_t)v.toInt();   // chan_key, 0 = none
    if (ck != 0 && chan_index_from_key(ck) < 0) { srv->send(400, "text/plain", "Unknown channel"); return; }
    secondary_chan = ck;
    cfg_put_ushort("sm2", secondary_chan);
    srv->send(200, "text/plain", String((int)secondary_chan));
  } else if (name == "tpsync") {
    trimpot_theme_sync = !trimpot_theme_sync;
    cfg_put_bool("tpsync", trimpot_theme_sync);
    if (trimpot_theme_sync && last_trimpot3 >= 0 && last_trimpot3 < THEME_SLOTS) {
      active_theme = (uint8_t)last_trimpot3;
      cfg_put_uint("atheme", active_theme);
      theme_to_globals(active_theme);
      flag_theme_update = true;
    }
    srv->send(200, "text/plain", trimpot_theme_sync ? "1" : "0");
  } else if (name == "name") {
    String n = v; n.trim();
    if (n.length() > 0 && n.length() <= 20) {
      device_name = n;
      cfg_put_string("devname", device_name);
      srv->send(200, "text/plain", "OK");
      reboot_at_ms = millis() + 400;  // restart (from loop) to apply new AP SSID
    } else {
      srv->send(400, "text/plain", "Name must be 1-20 characters");
    }
  } else if (name == "reboot") {
    srv->send(200, "text/plain", "OK");
    reboot_at_ms = millis() + 400;
  } else {
    srv->send(404, "text/plain", "Unknown action");
  }
}

void api_register(WebServer& server) {
  srv = &server;
  server.on("/api/state", HTTP_GET, apiState);
  server.on("/api/themes", HTTP_GET, apiThemesList);
  server.on(UriBraces("/api/themes/{}"), HTTP_GET, apiThemeGet);
  server.on(UriBraces("/api/themes/{}"), HTTP_POST, apiThemeSet);
  server.on(UriBraces("/api/themes/{}/activate"), HTTP_POST, apiThemeActivate);
  server.on(UriBraces("/api/themes/{}/copy"), HTTP_POST, apiThemeCopy);
  server.on("/api/channels", HTTP_GET, apiChannels);
  server.on("/api/config", HTTP_GET, apiConfigGet);
  server.on("/api/config", HTTP_POST, apiConfigSet);
  server.on("/api/fleet", HTTP_GET, apiFleet);
  server.on(UriBraces("/api/fleet/{}/mode"), HTTP_POST, apiFleetMode);
  server.on(UriBraces("/api/fleet/{}/theme"), HTTP_POST, apiFleetTheme);
  server.on(UriBraces("/api/fleet/{}/config"), HTTP_POST, apiFleetConfig);
  server.on(UriBraces("/api/fleet/{}/identify"), HTTP_POST, apiFleetIdentify);
  server.on(UriBraces("/api/action/{}"), HTTP_POST, apiAction);
}
