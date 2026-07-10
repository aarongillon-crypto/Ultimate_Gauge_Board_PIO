#include "web_server.h"
#include "api.h"
#include "assets_gen.h"
#include "../config_store.h"
#include "../fleet.h"
#include "Display_ST7701.h"
#include "esp_lcd_panel_rgb.h"
#include <WiFi.h>
#include <WebServer.h>
#include <esp_wifi.h>
#include <ArduinoOTA.h>
#include <Update.h>

#define WIFI_CHANNEL 1

static WebServer server(80);

// ---------- Static SPA assets (gzipped into flash by tools/build_web.py) ----------
// ETag is a content hash of the sources; Cache-Control: no-cache makes browsers
// revalidate each load, so after an OTA they get a fresh copy automatically (304
// when unchanged, full body when the firmware shipped new assets).
static void send_asset(const uint8_t* data, size_t len, const char* mime) {
  if (server.header("If-None-Match") == ASSET_ETAG) {
    server.send(304);
    return;
  }
  server.sendHeader("Content-Encoding", "gzip");
  server.sendHeader("ETag", ASSET_ETAG);
  server.sendHeader("Cache-Control", "no-cache");
  server.send_P(200, mime, (PGM_P)data, len);
}

// ---------- Snapshot / preview (kept from the legacy UI) ----------
// Capture the live RGB framebuffer and serve it as a BMP image.
// Row-streamed: converts + sends one row at a time from a static buffer, so a
// snapshot no longer allocates ~691KB of PSRAM per request. Rate-limited since
// each request still blocks the loop (LVGL) for the duration of the send.
// The framebuffer is read unlocked while the panel scans it — occasional
// tearing is accepted for a diagnostic view; do NOT lock against the vsync path.
static void handleSnapshot() {
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
static void handlePreview() {
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

// ---------- OTA ----------
// Belt-and-braces recovery page: a tiny hardcoded C string that depends on
// nothing (no SPA assets, no String building). If a bad build ever ships
// broken web assets, this page still flashes firmware.
static const char OTA_FALLBACK_HTML[] PROGMEM =
    "<!DOCTYPE html><html><body style='font-family:sans-serif;background:#222;color:#eee;text-align:center;padding:24px'>"
    "<h3>Recovery OTA</h3>"
    "<form method='POST' action='/ota' enctype='multipart/form-data'>"
    "<input type='file' name='firmware' accept='.bin'><br><br>"
    "<button type='submit' style='padding:10px 24px'>Flash Firmware</button>"
    "</form></body></html>";

static void handleOTAFallback() {
    server.send_P(200, "text/html", OTA_FALLBACK_HTML);
}

static void handleOTAPage() {
    String html = "<html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
    html += "<style>body{font-family:sans-serif;text-align:center;background:#222;color:#fff;padding:20px;}";
    html += "input,button{font-size:16px;padding:10px;margin:10px;border-radius:5px;border:none;}";
    html += "button{background:#e65100;color:white;width:200px;cursor:pointer;}</style></head><body>";
    html += "<h2>" + device_name + " - Firmware Update</h2>";
    html += "<p style='color:#aaa'>Select a .bin file built for this device. v" FIRMWARE_VERSION "</p>";
    html += "<form method='POST' action='/ota' enctype='multipart/form-data'>";
    html += "<input type='file' name='firmware' accept='.bin'><br>";
    html += "<button type='submit'>Flash Firmware</button>";
    html += "</form><p><a href='/' style='color:#aaa'>Back</a></p></body></html>";
    server.send(200, "text/html", html);
}

static void handleOTAUpload() {
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

static void handleOTADone() {
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

  // Web server + OTA are the recovery path — bring them up BEFORE ESP-NOW so a
  // fleet-sync init failure can never take down the ability to re-flash.
  static const char* hdrs[] = { "If-None-Match" };
  server.collectHeaders(hdrs, 1);   // needed for ETag/304 handling

  // SPA assets
  server.on("/", HTTP_GET, [](){ send_asset(ASSET_INDEX_HTML_GZ, ASSET_INDEX_HTML_GZ_LEN, "text/html"); });
  server.on("/app.js", HTTP_GET, [](){ send_asset(ASSET_APP_JS_GZ, ASSET_APP_JS_GZ_LEN, "application/javascript"); });
  server.on("/style.css", HTTP_GET, [](){ send_asset(ASSET_STYLE_CSS_GZ, ASSET_STYLE_CSS_GZ_LEN, "text/css"); });

  // JSON API
  api_register(server);

  // Diagnostics + OTA (recovery endpoints)
  server.on("/preview", handlePreview);
  server.on("/snapshot", handleSnapshot);
  server.on("/otafallback", handleOTAFallback);
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
  fleet_init();
}

void web_pump() {
  server.handleClient();
  ArduinoOTA.handle();
}
