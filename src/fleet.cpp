#include "fleet.h"
#include "themes.h"
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>

// ---------------- protocol v2 wire format ----------------
// Serialized field-by-field into packed structs — never memcpy GaugeTheme
// (its natural layout has trailing padding). All sizes well under ESP-NOW's
// 250-byte limit.
#define FLEET_MAGIC 0xA7
#define FLEET_PROTO 2
enum : uint8_t {
  PKT_PRESENCE2   = 0x10,
  PKT_THEME_FULL  = 0x11,
  PKT_CONFIG_SYNC = 0x12,
  PKT_CMD         = 0x13,
};
enum : uint8_t { FCMD_IDENTIFY = 0 };

typedef struct __attribute__((packed)) { uint8_t magic, proto, type, flags; } PktHeader;

typedef struct __attribute__((packed)) {
  PktHeader h;
  uint8_t mode, active_theme;
  uint8_t fw_major, fw_minor, fw_patch;
  uint16_t caps;              // reserved capability bits
  char name[21];
} PktPresence2;               // 32 B

typedef struct __attribute__((packed)) {
  PktHeader h;
  uint8_t slot, apply;        // apply: 0 = store only, 1 = store + activate
  uint32_t text, low, mid, high, bg, ml, li, nd, pk, g2, g3;
  uint8_t gt, gs; uint16_t ga;
  char name[21];
} PktThemeFull;               // 75 B

typedef struct __attribute__((packed)) {
  PktHeader h;
  float mmin[4], mmax[4], z1[4], z2[4];
  float smoothing, max_rate;
  uint32_t peak_hold_ms;
} PktConfigSync;              // 80 B

typedef struct __attribute__((packed)) { PktHeader h; uint8_t cmd; int32_t value; } PktCmd;  // 9 B

// ---------------- peer table ----------------
static PeerGauge fleet[10];
int fleet_count = 0;
static portMUX_TYPE fleet_mux = portMUX_INITIALIZER_UNLOCKED;

static uint8_t my_sta_mac[6] = {0};
static uint8_t my_ap_mac[6]  = {0};

int fleet_snapshot(PeerGauge *out, int max_entries) {
  taskENTER_CRITICAL(&fleet_mux);
  int n = (fleet_count < max_entries) ? fleet_count : max_entries;
  memcpy(out, fleet, n * sizeof(PeerGauge));
  taskEXIT_CRITICAL(&fleet_mux);
  return n;
}

// Insert/update a peer. v1 presence carries only mode (proto=1, no name);
// v2 presence fills the identity fields. Never downgrade proto — a v2 peer
// still dual-sending v1 must keep its v2 status.
static void update_peer(const uint8_t *mac, int mode, uint8_t proto,
                        uint8_t slot, const uint8_t fw[3], const char *name) {
  bool added = false;
  taskENTER_CRITICAL(&fleet_mux);
  PeerGauge *p = nullptr;
  for (int i = 0; i < fleet_count; i++) {
    if (memcmp(fleet[i].mac, mac, 6) == 0) { p = &fleet[i]; break; }
  }
  if (!p && fleet_count < 10) {
    p = &fleet[fleet_count++];
    memset(p, 0, sizeof(*p));
    memcpy(p->mac, mac, 6);
    added = true;
  }
  if (p) {
    p->mode = mode; p->last_seen = millis();
    if (proto > p->proto) p->proto = proto;
    if (proto >= 2) {
      p->active_theme = slot;
      if (fw) { p->fw[0] = fw[0]; p->fw[1] = fw[1]; p->fw[2] = fw[2]; }
      if (name) { strncpy(p->name, name, sizeof(p->name) - 1); p->name[sizeof(p->name)-1] = 0; }
    }
  }
  taskEXIT_CRITICAL(&fleet_mux);
  if (added) flag_new_peer = true;
}

// True if any live peer hasn't announced v2 yet (drives dual-send presence).
static bool any_legacy_peer() {
  bool legacy = false;
  taskENTER_CRITICAL(&fleet_mux);
  for (int i = 0; i < fleet_count; i++) {
    if (millis() - fleet[i].last_seen < 10000 && fleet[i].proto < 2) { legacy = true; break; }
  }
  taskEXIT_CRITICAL(&fleet_mux);
  return legacy;
}

static bool peer_is_v2(const uint8_t *mac) {
  bool v2 = false;
  taskENTER_CRITICAL(&fleet_mux);
  for (int i = 0; i < fleet_count; i++) {
    if (memcmp(fleet[i].mac, mac, 6) == 0) { v2 = fleet[i].proto >= 2; break; }
  }
  taskEXIT_CRITICAL(&fleet_mux);
  return v2;
}

// ---------------- staged behavior config (CONFIG_SYNC in) ----------------
static BehaviorConfig s_pending_cfg;
static volatile bool s_pending_cfg_valid = false;
static portMUX_TYPE s_cfg_mux = portMUX_INITIALIZER_UNLOCKED;

bool fleet_take_pending_config(BehaviorConfig *out) {
  if (!s_pending_cfg_valid) return false;
  taskENTER_CRITICAL(&s_cfg_mux);
  *out = s_pending_cfg;
  s_pending_cfg_valid = false;
  taskEXIT_CRITICAL(&s_cfg_mux);
  return true;
}

// ---------------- receive ----------------
static void handle_v1(const uint8_t *mac, EspNowPacket *pkt) {
  if (pkt->type == 1) {
    update_peer(mac, pkt->mode, 1, 0, nullptr, nullptr);
  }
  else if (pkt->type == 2) {
    // Remote mode change — applied AND persisted from loop() (no NVS here).
    pending_mode = constrain(pkt->mode, 0, 3);
    flag_mode_update = true;
  }
  else if (pkt->type == 3) {
    PendingTheme p = {}; p.op = PT_DYNAMIC_COLORS;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3; p.c4 = pkt->c4;
    theme_stage(&p);
  }
  else if (pkt->type == 4) {
    test_mode_enabled = (pkt->value == 1);
  }
  else if (pkt->type == 5) {
    pending_brightness = constrain(pkt->value, 10, 100);
    flag_bright_update = true;
  }
  else if (pkt->type == 6) {
    show_perf_stats = (pkt->value == 1);
    flag_stats_update = true;
  }
  else if (pkt->type == 7) {
    PendingTheme p = {}; p.op = PT_UI_COLORS;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3; p.c4 = pkt->c4;
    p.c5 = (uint32_t)pkt->value;
    theme_stage(&p);
  }
  else if (pkt->type == 8) {
    uint32_t v = (uint32_t)pkt->value;
    PendingTheme p = {}; p.op = PT_GRADIENT;
    p.c1 = pkt->c1; p.c2 = pkt->c2; p.c3 = pkt->c3;
    p.gt = (uint8_t)(v & 0x0F);
    p.gs = (uint8_t)((v >> 4) & 0x0F);
    p.ga = (uint16_t)((v >> 8) & 0xFFFF);
    theme_stage(&p);
  }
}

static void handle_v2(const uint8_t *mac, const uint8_t *data, int len) {
  const PktHeader *h = (const PktHeader *)data;
  switch (h->type) {
    case PKT_PRESENCE2: {
      if (len < (int)sizeof(PktPresence2)) return;
      const PktPresence2 *p = (const PktPresence2 *)data;
      uint8_t fw[3] = { p->fw_major, p->fw_minor, p->fw_patch };
      char name[21]; memcpy(name, p->name, 21); name[20] = 0;
      update_peer(mac, constrain((int)p->mode, 0, 3), 2, p->active_theme, fw, name);
      break;
    }
    case PKT_THEME_FULL: {
      if (len < (int)sizeof(PktThemeFull)) return;
      const PktThemeFull *p = (const PktThemeFull *)data;
      if (p->slot >= THEME_SLOTS) return;
      PendingTheme t = {}; t.op = PT_FULL_THEME;
      t.slot = p->slot; t.apply = p->apply ? 1 : 0;
      t.full.text = p->text; t.full.low = p->low; t.full.mid = p->mid; t.full.high = p->high;
      t.full.background = p->bg; t.full.mode_label = p->ml; t.full.link_icon = p->li;
      t.full.needle = p->nd; t.full.peak = p->pk;
      t.full.bg_grad2 = p->g2; t.full.bg_grad3 = p->g3;
      t.full.bg_grad_type = constrain((int)p->gt, 0, 5);
      t.full.bg_grad_stops = (p->gs == 3) ? 3 : 2;
      t.full.bg_grad_angle = p->ga % 361;
      memcpy(t.fname, p->name, 21); t.fname[20] = 0;
      theme_stage(&t);
      break;
    }
    case PKT_CONFIG_SYNC: {
      if (len < (int)sizeof(PktConfigSync)) return;
      const PktConfigSync *p = (const PktConfigSync *)data;
      taskENTER_CRITICAL(&s_cfg_mux);
      for (int i = 0; i < 4; i++) {
        s_pending_cfg.mode[i].min = p->mmin[i]; s_pending_cfg.mode[i].max = p->mmax[i];
        s_pending_cfg.mode[i].z1  = p->z1[i];   s_pending_cfg.mode[i].z2  = p->z2[i];
      }
      s_pending_cfg.smoothing = p->smoothing;
      s_pending_cfg.max_rate = p->max_rate;
      s_pending_cfg.peak_hold_ms = p->peak_hold_ms;
      s_pending_cfg_valid = true;
      taskEXIT_CRITICAL(&s_cfg_mux);
      break;
    }
    case PKT_CMD: {
      if (len < (int)sizeof(PktCmd)) return;
      const PktCmd *p = (const PktCmd *)data;
      if (p->cmd == FCMD_IDENTIFY) identify_end_ms = millis() + 1500;  // loop blinks the backlight
      break;
    }
    default: break;
  }
}

static void OnDataRecv(const esp_now_recv_info_t * info, const uint8_t *incomingData, int len) {
  const uint8_t* mac = info->src_addr;
  if (len < 1) return;
  if (memcmp(mac, my_sta_mac, 6) == 0 || memcmp(mac, my_ap_mac, 6) == 0) return;  // own echo

  if (debug_mode_enabled) {
    Serial.printf("[ESP-NOW] first=0x%02X len=%d from %02X:%02X:%02X:%02X:%02X:%02X\n",
      incomingData[0], len, mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  }

  if (incomingData[0] == FLEET_MAGIC && len >= (int)sizeof(PktHeader)) {
    handle_v2(mac, incomingData, len);
  } else if (len == (int)sizeof(EspNowPacket) && incomingData[0] >= 1 && incomingData[0] <= 8) {
    handle_v1(mac, (EspNowPacket *)incomingData);
  }
}

// ---------------- send ----------------
// All sends use the AP interface on channel 0 (current) — keep this convention
// for every peer registration or unicast sends silently fail.
static void ensure_peer(const uint8_t *mac) {
  esp_now_peer_info_t peerInfo = {};
  memcpy(peerInfo.peer_addr, mac, 6);
  peerInfo.channel = 0;
  peerInfo.encrypt = false;
  peerInfo.ifidx = WIFI_IF_AP;
  if (!esp_now_is_peer_exist(mac)) esp_now_add_peer(&peerInfo);
}

static const uint8_t BCAST[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void fleet_send(const uint8_t *mac, const void *buf, size_t len) {
  const uint8_t *target = mac ? mac : BCAST;
  ensure_peer(target);
  esp_now_send(target, (const uint8_t *)buf, len);
}

void broadcast_packet(EspNowPacket *pkt) {
  fleet_send(nullptr, pkt, sizeof(EspNowPacket));
}

// Presence tick (every 2s from loop). Always announce v2; additionally send
// the v1 presence while any live peer is still legacy (or nobody has spoken
// yet) so un-updated gauges keep seeing us during a rolling OTA.
void broadcast_presence() {
  PktPresence2 p = {};
  p.h.magic = FLEET_MAGIC; p.h.proto = FLEET_PROTO; p.h.type = PKT_PRESENCE2;
  p.mode = (uint8_t)current_mode;
  p.active_theme = active_theme;
  p.fw_major = FIRMWARE_VER_MAJOR; p.fw_minor = FIRMWARE_VER_MINOR; p.fw_patch = FIRMWARE_VER_PATCH;
  strncpy(p.name, device_name.c_str(), sizeof(p.name) - 1);
  fleet_send(nullptr, &p, sizeof(p));

  if (fleet_count == 0 || any_legacy_peer()) {
    EspNowPacket pkt = {}; pkt.type = 1; pkt.mode = current_mode;
    broadcast_packet(&pkt);
  }
}

void send_remote_command(uint8_t *targetMac, int newMode) {
  EspNowPacket pkt = {}; pkt.type = 2; pkt.mode = newMode;
  fleet_send(targetMac, &pkt, sizeof(pkt));
}

// Push a local theme slot. Unicast to a legacy peer falls back to the v1
// triple (3+7+8) — colours land in the peer's ACTIVE slot (no slot placement,
// no name); the web UI badges legacy peers so this is discoverable.
void fleet_push_theme(const uint8_t *mac, uint8_t slot, bool activate) {
  if (slot >= THEME_SLOTS) return;
  const GaugeTheme &t = themes[slot];

  if (mac && !peer_is_v2(mac)) {
    EspNowPacket p = {};
    p.type = 3; p.c1 = t.text; p.c2 = t.low; p.c3 = t.mid; p.c4 = t.high;
    fleet_send(mac, &p, sizeof(p));
    p = {}; p.type = 7; p.c1 = t.background; p.c2 = t.mode_label;
    p.c3 = t.link_icon; p.c4 = t.needle; p.value = (int)t.peak;
    fleet_send(mac, &p, sizeof(p));
    p = {}; p.type = 8; p.c1 = t.bg_grad2; p.c2 = t.bg_grad3; p.c3 = t.background;
    p.value = (int)((uint32_t)t.bg_grad_type | ((uint32_t)t.bg_grad_stops << 4) | ((uint32_t)t.bg_grad_angle << 8));
    fleet_send(mac, &p, sizeof(p));
    return;
  }

  PktThemeFull p = {};
  p.h.magic = FLEET_MAGIC; p.h.proto = FLEET_PROTO; p.h.type = PKT_THEME_FULL;
  p.slot = slot; p.apply = activate ? 1 : 0;
  p.text = t.text; p.low = t.low; p.mid = t.mid; p.high = t.high;
  p.bg = t.background; p.ml = t.mode_label; p.li = t.link_icon;
  p.nd = t.needle; p.pk = t.peak;
  p.g2 = t.bg_grad2; p.g3 = t.bg_grad3;
  p.gt = t.bg_grad_type; p.gs = t.bg_grad_stops; p.ga = t.bg_grad_angle;
  strncpy(p.name, theme_names[slot].c_str(), sizeof(p.name) - 1);
  fleet_send(mac, &p, sizeof(p));
}

void fleet_push_config(const uint8_t *mac) {
  PktConfigSync p = {};
  p.h.magic = FLEET_MAGIC; p.h.proto = FLEET_PROTO; p.h.type = PKT_CONFIG_SYNC;
  for (int i = 0; i < 4; i++) {
    p.mmin[i] = behavior.mode[i].min; p.mmax[i] = behavior.mode[i].max;
    p.z1[i] = behavior.mode[i].z1;    p.z2[i] = behavior.mode[i].z2;
  }
  p.smoothing = behavior.smoothing;
  p.max_rate = behavior.max_rate;
  p.peak_hold_ms = behavior.peak_hold_ms;
  fleet_send(mac, &p, sizeof(p));
}

void fleet_send_identify(const uint8_t *mac) {
  PktCmd p = {};
  p.h.magic = FLEET_MAGIC; p.h.proto = FLEET_PROTO; p.h.type = PKT_CMD;
  p.cmd = FCMD_IDENTIFY;
  fleet_send(mac, &p, sizeof(p));
}

void fleet_init() {
  WiFi.macAddress(my_sta_mac);
  WiFi.softAPmacAddress(my_ap_mac);

  // Fleet sync is optional; web/OTA (already running) must survive its failure.
  if (esp_now_init() == ESP_OK) {
    esp_now_register_recv_cb(OnDataRecv);
  } else {
    Serial.println("[ESPNOW] init FAILED — fleet sync disabled this boot");
  }
}
