#include "layout_store.h"
#include <LittleFS.h>

static const char* LAYOUT_PATH = "/layout.json";
static const char* TEMP_PATH   = "/layout.tmp";
static bool s_mounted = false;

bool layout_store_init() {
  // true = format on failed mount. The spiffs partition has never held data
  // before this feature, so formatting a virgin/corrupt partition is safe.
  s_mounted = LittleFS.begin(true);
  if (!s_mounted) Serial.println("[LAYOUT] LittleFS mount FAILED — layouts disabled, default face only");
  else Serial.printf("[LAYOUT] LittleFS mounted (%u KB used / %u KB)\n",
                     (unsigned)(LittleFS.usedBytes() / 1024), (unsigned)(LittleFS.totalBytes() / 1024));
  return s_mounted;
}

bool layout_store_present() {
  return s_mounted && LittleFS.exists(LAYOUT_PATH);
}

String layout_store_read() {
  if (!layout_store_present()) return String();
  File f = LittleFS.open(LAYOUT_PATH, "r");
  if (!f) return String();
  String s = f.readString();
  f.close();
  return s;
}

bool layout_store_write(const String& json) {
  if (!s_mounted) return false;
  File f = LittleFS.open(TEMP_PATH, "w");
  if (!f) return false;
  size_t n = f.print(json);
  f.close();
  if (n != json.length()) { LittleFS.remove(TEMP_PATH); return false; }
  LittleFS.remove(LAYOUT_PATH);              // rename() won't overwrite
  return LittleFS.rename(TEMP_PATH, LAYOUT_PATH);
}

void layout_store_delete() {
  if (s_mounted) LittleFS.remove(LAYOUT_PATH);
}
