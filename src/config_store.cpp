#include "config_store.h"
#include "haltech_channels.h"   // units_* display preferences
#include <Preferences.h>

static Preferences preferences;   // sole instance in the firmware
static TaskHandle_t s_loop_task = nullptr;

void cfg_set_loop_task() { s_loop_task = xTaskGetCurrentTaskHandle(); }

// Loud diagnostic (not a crash) if a write sneaks in from the wrong task —
// blocking flash writes in the ESP-NOW/CAN task context are the bug class
// that broke "Apply to ALL" for months.
static void guard_task() {
  if (s_loop_task && xTaskGetCurrentTaskHandle() != s_loop_task) {
    Serial.printf("[CFG][BUG] NVS write from task '%s' — writes must run in loop()!\n",
                  pcTaskGetName(NULL));
  }
}

void cfg_put_int(const char* key, int v)            { guard_task(); preferences.begin("gauge", false); preferences.putInt(key, v);    preferences.end(); }
void cfg_put_uint(const char* key, uint32_t v)      { guard_task(); preferences.begin("gauge", false); preferences.putUInt(key, v);   preferences.end(); }
void cfg_put_uchar(const char* key, uint8_t v)      { guard_task(); preferences.begin("gauge", false); preferences.putUChar(key, v);  preferences.end(); }
void cfg_put_ushort(const char* key, uint16_t v)    { guard_task(); preferences.begin("gauge", false); preferences.putUShort(key, v); preferences.end(); }
void cfg_put_bool(const char* key, bool v)          { guard_task(); preferences.begin("gauge", false); preferences.putBool(key, v);   preferences.end(); }
void cfg_put_string(const char* key, const String& v) { guard_task(); preferences.begin("gauge", false); preferences.putString(key, v); preferences.end(); }

// NVS key for one slot field, e.g. "t2_bg" (stays well under the 15-char limit).
static void theme_key(char *buf, uint8_t slot, const char *field) {
  snprintf(buf, 8, "t%u_%s", slot, field);
}

void cfg_persist_theme_slot(uint8_t slot, const GaugeTheme& t) {
  if (slot >= THEME_SLOTS) return;
  guard_task();
  char k[8];
  preferences.begin("gauge", false);
  theme_key(k,slot,"tx"); preferences.putUInt(k, t.text);
  theme_key(k,slot,"lo"); preferences.putUInt(k, t.low);
  theme_key(k,slot,"mi"); preferences.putUInt(k, t.mid);
  theme_key(k,slot,"hi"); preferences.putUInt(k, t.high);
  theme_key(k,slot,"bg"); preferences.putUInt(k, t.background);
  theme_key(k,slot,"ml"); preferences.putUInt(k, t.mode_label);
  theme_key(k,slot,"li"); preferences.putUInt(k, t.link_icon);
  theme_key(k,slot,"nd"); preferences.putUInt(k, t.needle);
  theme_key(k,slot,"pk"); preferences.putUInt(k, t.peak);
  theme_key(k,slot,"b2"); preferences.putUInt(k, t.bg_grad2);
  theme_key(k,slot,"b3"); preferences.putUInt(k, t.bg_grad3);
  theme_key(k,slot,"gt"); preferences.putUChar(k, t.bg_grad_type);
  theme_key(k,slot,"gs"); preferences.putUChar(k, t.bg_grad_stops);
  theme_key(k,slot,"ga"); preferences.putUShort(k, t.bg_grad_angle);
  preferences.end();
}

void cfg_load_theme_slot(uint8_t slot, const GaugeTheme& d, GaugeTheme* out) {
  if (slot >= THEME_SLOTS || !out) return;
  char k[8];
  preferences.begin("gauge", true);
  theme_key(k,slot,"tx"); out->text       = preferences.getUInt(k, d.text);
  theme_key(k,slot,"lo"); out->low        = preferences.getUInt(k, d.low);
  theme_key(k,slot,"mi"); out->mid        = preferences.getUInt(k, d.mid);
  theme_key(k,slot,"hi"); out->high       = preferences.getUInt(k, d.high);
  theme_key(k,slot,"bg"); out->background = preferences.getUInt(k, d.background);
  theme_key(k,slot,"ml"); out->mode_label = preferences.getUInt(k, d.mode_label);
  theme_key(k,slot,"li"); out->link_icon  = preferences.getUInt(k, d.link_icon);
  theme_key(k,slot,"nd"); out->needle     = preferences.getUInt(k, d.needle);
  theme_key(k,slot,"pk"); out->peak       = preferences.getUInt(k, d.peak);
  theme_key(k,slot,"b2"); out->bg_grad2   = preferences.getUInt(k, d.bg_grad2);
  theme_key(k,slot,"b3"); out->bg_grad3   = preferences.getUInt(k, d.bg_grad3);
  theme_key(k,slot,"gt"); out->bg_grad_type  = preferences.getUChar(k, d.bg_grad_type);
  theme_key(k,slot,"gs"); out->bg_grad_stops = preferences.getUChar(k, d.bg_grad_stops);
  theme_key(k,slot,"ga"); out->bg_grad_angle = preferences.getUShort(k, d.bg_grad_angle);
  preferences.end();
}

void cfg_put_theme_name(uint8_t slot, const String& name) {
  if (slot >= THEME_SLOTS) return;
  guard_task();
  char k[8]; theme_key(k, slot, "nm");
  preferences.begin("gauge", false); preferences.putString(k, name); preferences.end();
}

String cfg_load_theme_name(uint8_t slot, const char* dflt) {
  if (slot >= THEME_SLOTS) return String(dflt);
  char k[8]; theme_key(k, slot, "nm");
  preferences.begin("gauge", true);
  String v = preferences.getString(k, dflt);
  preferences.end();
  return v;
}

void cfg_disable_gradient(uint8_t slot) {
  guard_task();
  preferences.begin("gauge", false);
  preferences.putUChar("cgt", 0);
  char k[8]; theme_key(k, slot, "gt"); preferences.putUChar(k, 0);
  preferences.end();
}

// Behavior keys: m{mode}_{field}, e.g. "m2_z1" (well under the 15-char limit).
static void behavior_key(char* buf, uint8_t mode, const char* field) {
  snprintf(buf, 10, "m%u_%s", mode, field);
}

void cfg_persist_behavior(const BehaviorConfig& b) {
  guard_task();
  char k[10];
  preferences.begin("gauge", false);
  for (uint8_t i = 0; i < 4; i++) {
    behavior_key(k,i,"ch");  preferences.putUShort(k, b.mode[i].chan_key);
    behavior_key(k,i,"lbl"); preferences.putString(k, b.mode[i].label);
    behavior_key(k,i,"min"); preferences.putFloat(k, b.mode[i].min);
    behavior_key(k,i,"max"); preferences.putFloat(k, b.mode[i].max);
    behavior_key(k,i,"z1");  preferences.putFloat(k, b.mode[i].z1);
    behavior_key(k,i,"z2");  preferences.putFloat(k, b.mode[i].z2);
  }
  preferences.putFloat("smooth", b.smoothing);
  preferences.putFloat("maxrate", b.max_rate);
  preferences.putUInt("pkms", b.peak_hold_ms);
  preferences.end();
}

void cfg_load_behavior(BehaviorConfig* out) {
  char k[10];
  const BehaviorConfig& d = BEHAVIOR_DEFAULTS;
  preferences.begin("gauge", true);
  for (uint8_t i = 0; i < 4; i++) {
    behavior_key(k,i,"ch");  out->mode[i].chan_key = preferences.getUShort(k, d.mode[i].chan_key);
    behavior_key(k,i,"lbl");
    String lbl = preferences.getString(k, d.mode[i].label);
    strncpy(out->mode[i].label, lbl.c_str(), sizeof(out->mode[i].label) - 1);
    out->mode[i].label[sizeof(out->mode[i].label) - 1] = 0;
    behavior_key(k,i,"min"); out->mode[i].min = preferences.getFloat(k, d.mode[i].min);
    behavior_key(k,i,"max"); out->mode[i].max = preferences.getFloat(k, d.mode[i].max);
    behavior_key(k,i,"z1");  out->mode[i].z1  = preferences.getFloat(k, d.mode[i].z1);
    behavior_key(k,i,"z2");  out->mode[i].z2  = preferences.getFloat(k, d.mode[i].z2);
  }
  out->smoothing    = preferences.getFloat("smooth", d.smoothing);
  out->max_rate     = preferences.getFloat("maxrate", d.max_rate);
  out->peak_hold_ms = preferences.getUInt("pkms", d.peak_hold_ms);
  preferences.end();
}

void cfg_mark_boot_started() { guard_task(); preferences.begin("gauge", false); preferences.putBool("bootok", false); preferences.end(); }
void cfg_mark_boot_ok()      { guard_task(); preferences.begin("gauge", false); preferences.putBool("bootok", true);  preferences.end(); }

bool cfg_load_all(const char* defaultName) {
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
  dim_brightness = preferences.getInt("dimbr", 20);
  dim_can_enabled = preferences.getBool("dimen", false);
  dim_source = preferences.getUChar("dimsrc", DIM_SRC_EITHER);
  peak_hold_enabled = preferences.getBool("peak", true);
  debug_mode_enabled = preferences.getBool("dbg", false);
  current_font = (uint8_t)preferences.getUInt("font", 0);
  device_name = preferences.getString("devname", defaultName);
  secondary_chan = preferences.getUShort("sm2", 0);   // chan_key (old "sm" index key retired)
  units_press_psi  = preferences.getBool("u_psi",  true);
  units_temp_f     = preferences.getBool("u_degf", false);
  units_speed_mph  = preferences.getBool("u_mph",  false);
  units_lambda_afr = preferences.getBool("u_afr",  true);
  current_page = (DisplayPage)preferences.getUInt("page", 0);
  active_theme = (uint8_t)preferences.getUInt("atheme", 0);
  trimpot_theme_sync = preferences.getBool("tpsync", false);
  layout_enabled = preferences.getBool("luse", true);
  // Crash-safe boot flag: set false before risky rendering, true once we light
  // the panel. If it's still false here, the previous boot crashed/hung mid-render.
  bool last_boot_completed = preferences.getBool("bootok", true);
  preferences.end();
  if (active_theme >= THEME_SLOTS) active_theme = 0;
  return last_boot_completed;
}
