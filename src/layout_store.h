// Persistence for uploaded layout documents (Layout_Engine_Spec.md §7).
// The layout JSON lives as a file on LittleFS (mounted on the existing,
// previously-unused "spiffs" partition — no repartition needed). The
// compiled-in default face is always retained: no/invalid/disabled layout
// simply means the built-in gauge page renders instead.
#pragma once
#include <Arduino.h>

// Mount LittleFS (format-on-fail: the partition was never used before this
// feature, so there is nothing to lose). Call once in setup().
bool layout_store_init();

bool layout_store_present();                 // a layout file exists
String layout_store_read();                  // "" if none/unreadable
bool layout_store_write(const String& json); // atomic-ish: temp file + rename
void layout_store_delete();
