// =============================================================================
// M5Stack Tab5 — Phase 0 ESP-NOW feasibility spike
// =============================================================================
// Goal: prove ESP-NOW works on the Tab5 (P4 + C6) against the existing gauges.
//
//  RECEIVE test: the gauges already broadcast a presence packet (type 1) every
//  2 s on channel 1. This sketch listens and prints every ESP-NOW frame it
//  hears — if presence packets appear, receive works.
//
//  SEND test: every 5 s it broadcasts a "test mode ON/OFF" command (type 4),
//  the same packet the web UI / fleet uses. A gauge in range should visibly
//  toggle its Test Mode — proving the send direction end to end.
//
// This is throwaway spike code; the real monitor firmware comes later. All
// output goes to Serial (USB) AND the Tab5 screen so you can read it on-device.
// =============================================================================

#include <M5Unified.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static const uint8_t GAUGE_CHANNEL = 1;   // gauges run softAP on channel 1
static const uint8_t BROADCAST[6] = {0xFF,0xFF,0xFF,0xFF,0xFF,0xFF};

// Must match the gauge's EspNowPacket exactly (src/main.cpp).
typedef struct __attribute__((packed)) {
  uint8_t  type;
  int      mode;
  uint32_t c1, c2, c3, c4;
  int      value;
} EspNowPacket;

static volatile uint32_t rx_count = 0;
static char last_line[96] = "(waiting for packets...)";

static void log_both(const char* s) {
  Serial.println(s);
  M5.Display.println(s);
}

// ESP-NOW receive callback (same signature the gauge uses).
void onRecv(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  rx_count++;
  const uint8_t* m = info->src_addr;
  int type = (len >= 1) ? data[0] : -1;
  snprintf(last_line, sizeof(last_line),
           "RX #%lu  type=%d len=%d  from %02X:%02X:%02X:%02X:%02X:%02X",
           (unsigned long)rx_count, type, len, m[0], m[1], m[2], m[3], m[4], m[5]);
  Serial.println(last_line);
}

static void send_test_mode(bool on) {
  EspNowPacket pkt = {};
  pkt.type  = 4;            // test-mode command
  pkt.value = on ? 1 : 0;
  esp_err_t e = esp_now_send(BROADCAST, (uint8_t*)&pkt, sizeof(pkt));
  Serial.printf("[TX] test=%d -> esp_now_send=%s\n", on ? 1 : 0, esp_err_to_name(e));
}

void setup() {
  auto cfg = M5.config();
  M5.begin(cfg);                 // brings up board, C6 link, antenna
  M5.Display.setTextSize(2);
  Serial.begin(115200);
  delay(300);
  log_both("Tab5 ESP-NOW spike");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();             // don't join an AP; we only need the radio up
  delay(100);

  // Lock to the gauges' channel so we can hear their broadcasts.
  esp_err_t ce = esp_wifi_set_channel(GAUGE_CHANNEL, WIFI_SECOND_CHAN_NONE);
  Serial.printf("[INIT] set_channel(%d)=%s\n", GAUGE_CHANNEL, esp_err_to_name(ce));

  if (esp_now_init() != ESP_OK) { log_both("esp_now_init FAILED"); return; }
  log_both("esp_now_init OK");
  esp_now_register_recv_cb(onRecv);

  // Add broadcast peer so we can send the test command.
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, BROADCAST, 6);
  peer.channel = GAUGE_CHANNEL;
  peer.encrypt = false;
  esp_now_add_peer(&peer);

  uint8_t mac[6]; WiFi.macAddress(mac);
  Serial.printf("[INIT] my STA MAC %02X:%02X:%02X:%02X:%02X:%02X, channel %d\n",
                mac[0],mac[1],mac[2],mac[3],mac[4],mac[5], GAUGE_CHANNEL);
  log_both("Listening on ch1...");
}

void loop() {
  M5.update();
  static uint32_t last_tx = 0, last_ui = 0;
  static bool test_on = false;
  uint32_t now = millis();

  if (now - last_tx > 5000) {           // toggle a gauge's test mode every 5 s
    last_tx = now;
    test_on = !test_on;
    send_test_mode(test_on);
  }

  if (now - last_ui > 500) {            // refresh on-screen status
    last_ui = now;
    M5.Display.setCursor(0, 120);
    M5.Display.printf("RX count: %lu      \n", (unsigned long)rx_count);
    M5.Display.printf("%s\n", last_line);
  }
  delay(10);
}
