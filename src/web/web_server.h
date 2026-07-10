// WiFi AP + web server + OTA. Handlers run in loopTask context (via
// server.handleClient()), so they may touch globals and persist via
// config_store directly. Legacy HTML UI — replaced by the SPA in Stage 3.
#pragma once
#include "../app_state.h"

// Bring up WiFi AP, HTTP routes, ArduinoOTA, then ESP-NOW (via fleet_init).
// Web/OTA start unconditionally — they are the recovery path.
void setup_wifi();

// Pump HTTP + ArduinoOTA (called from loop() and the pre-backlight render loop).
void web_pump();
