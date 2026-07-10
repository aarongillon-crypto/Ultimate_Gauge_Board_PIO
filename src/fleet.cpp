#include "fleet.h"
#include "themes.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

static PeerGauge fleet[10];
int fleet_count = 0;
// fleet[] is written from the ESP-NOW callback (WiFi task, core 0) and read by
// web handlers in loop() — guard both sides with a short critical section.
static portMUX_TYPE fleet_mux = portMUX_INITIALIZER_UNLOCKED;

// Own MACs, cached once in fleet_init() so the recv callback doesn't call into
// the WiFi driver per packet just to filter its own echoes.
static uint8_t my_sta_mac[6] = {0};
static uint8_t my_ap_mac[6]  = {0};

int fleet_snapshot(PeerGauge *out, int max_entries) {
  taskENTER_CRITICAL(&fleet_mux);
  int n = (fleet_count < max_entries) ? fleet_count : max_entries;
  memcpy(out, fleet, n * sizeof(PeerGauge));
  taskEXIT_CRITICAL(&fleet_mux);
  return n;
}

static void update_peer_list(const uint8_t *mac, int mode) {
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

static void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t *incomingData, int len) {
  const uint8_t* mac = info->src_addr;
  if (len != sizeof(EspNowPacket)) return;

  // Ignore our own packets echoed back via the AP interface (MACs cached at init)
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
    // Dynamic colours — staged; loop() applies to globals + slot + NVS.
    PendingTheme p = {}; p.op = PT_DYNAMIC_COLORS;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3; p.c4 = pkt->c4;
    theme_stage(&p);
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
    // Static/UI colours — staged (c5 carries peak colour from pkt->value).
    PendingTheme p = {}; p.op = PT_UI_COLORS;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3; p.c4 = pkt->c4;
    p.c5 = (uint32_t)pkt->value;
    theme_stage(&p);
  }
  else if (pkt->type == 8) {
    // Background gradient: c1/c2 = stops 2/3, c3 = background (stop 1),
    // value packs type|stops|angle (see handleGrad). Staged for loop().
    uint32_t v = (uint32_t)pkt->value;
    PendingTheme p = {}; p.op = PT_GRADIENT;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3;
    p.gt = (uint8_t)(v & 0x0F);
    p.gs = (uint8_t)((v >> 4) & 0x0F);
    p.ga = (uint16_t)((v >> 8) & 0xFFFF);
    theme_stage(&p);
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

void fleet_init() {
  // Cache our own MACs once — OnDataRecv filters self-echoes against these
  // instead of calling into the WiFi driver per packet.
  WiFi.macAddress(my_sta_mac);
  WiFi.softAPmacAddress(my_ap_mac);

  // Fleet sync is optional; web/OTA (already running) must survive its failure.
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
  } else {
    Serial.println("[ESPNOW] init FAILED — fleet sync disabled this boot");
  }
}
