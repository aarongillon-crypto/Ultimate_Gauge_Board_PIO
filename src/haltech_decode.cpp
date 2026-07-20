#include "haltech_decode.h"
#include "themes.h"
#include "GlowCraft_Driver.h"
#include "driver/twai.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>

extern QueueHandle_t canMsgQueue;   // created in can_rx.cpp

// ---------------- channel value store ----------------
#define MAX_CHANNELS 224            // >= HALTECH_CHANNEL_COUNT (asserted at init)
static volatile float    s_val[MAX_CHANNELS];
static volatile uint32_t s_seen[MAX_CHANNELS];   // millis() of last update, 0 = never

float haltech_value(int index) {
  if (index < 0 || index >= HALTECH_CHANNEL_COUNT) return 0.0f;
  return s_val[index];
}

uint32_t haltech_age_ms(int index) {
  if (index < 0 || index >= HALTECH_CHANNEL_COUNT) return UINT32_MAX;
  uint32_t seen = s_seen[index];
  if (!seen) return UINT32_MAX;
  return millis() - seen;
}

float haltech_value_by_key(uint16_t key) {
  return haltech_value(chan_index_from_key(key));
}

bool haltech_fresh_by_key(uint16_t key, uint32_t max_age_ms) {
  return haltech_age_ms(chan_index_from_key(key)) <= max_age_ms;
}

// ---------------- per-ID dispatch index ----------------
// The registry is grouped by CAN ID; build a compact (id, first, count) index
// once so each frame binary-searches ~50 IDs instead of scanning 160 rows.
typedef struct { uint16_t can_id; uint8_t first, count; } IdIndex;
static IdIndex s_index[80];
static int s_index_count = 0;

static void build_index() {
  s_index_count = 0;
  for (int i = 0; i < HALTECH_CHANNEL_COUNT; ) {
    uint16_t id = HALTECH_CHANNELS[i].can_id;
    int first = i;
    while (i < HALTECH_CHANNEL_COUNT && HALTECH_CHANNELS[i].can_id == id) i++;
    if (s_index_count < (int)(sizeof(s_index)/sizeof(s_index[0]))) {
      s_index[s_index_count++] = { id, (uint8_t)first, (uint8_t)(i - first) };
    }
  }
  if (HALTECH_CHANNEL_COUNT > MAX_CHANNELS) {
    Serial.println("[HALTECH][BUG] channel table exceeds MAX_CHANNELS!");
  }
}

static const IdIndex* find_id(uint16_t can_id) {
  int lo = 0, hi = s_index_count - 1;
  while (lo <= hi) {
    int mid = (lo + hi) / 2;
    if (s_index[mid].can_id < can_id) lo = mid + 1;
    else if (s_index[mid].can_id > can_id) hi = mid - 1;
    else return &s_index[mid];
  }
  return nullptr;
}

// ---------------- extraction (big-endian, PDF pages 3-4) ----------------
static float extract_channel(const HaltechChannel& c, const uint8_t* data, uint8_t dlc) {
  int32_t raw;
  if (c.bit_start != 0xFF) {
    // Bit-addressed within one byte: bit_start = MSB of the field.
    if (c.offset >= dlc) return NAN;
    uint8_t shift = c.bit_start - (c.bit_len - 1);
    raw = (data[c.offset] >> shift) & ((1u << c.bit_len) - 1);
  } else {
    if (c.offset + c.size > dlc) return NAN;
    if (c.size == 1) {
      raw = c.is_signed ? (int32_t)(int8_t)data[c.offset] : data[c.offset];
    } else if (c.size == 2) {
      uint16_t u = ((uint16_t)data[c.offset] << 8) | data[c.offset + 1];
      raw = c.is_signed ? (int32_t)(int16_t)u : (int32_t)u;
    } else { // 4
      uint32_t u = ((uint32_t)data[c.offset] << 24) | ((uint32_t)data[c.offset+1] << 16)
                 | ((uint32_t)data[c.offset+2] << 8) | data[c.offset+3];
      raw = (int32_t)u;   // 4-byte channels are counters/timers; sign per table
    }
  }
  return (float)raw * c.scale + c.bias;
}

// ---------------- special hooks ----------------
// Rotary Trim 3 (0x3E4 byte 6) drives live theme-slot switching when trimpot
// sync is on — staged to loop(), same rules as always (no globals/LVGL here).
static void hook_3e4_trimpot(const twai_message_t* m) {
  if (m->data_length_code <= 6) return;
  int pos = m->data[6];
  if (pos >= 0 && pos < THEME_SLOTS && pos != last_trimpot3) {
    last_trimpot3 = pos;
    if (trimpot_theme_sync) {
      PendingTheme p = {}; p.op = PT_ACTIVATE_SLOT; p.slot = (uint8_t)pos;
      theme_stage(&p);
    }
  }
}

// ---------------- non-Haltech CAN sniffer (debug) ----------------
#ifdef EVO_SNIFFER
#define SNIFF_SLOTS 48    // a whole car bus has many more distinct IDs than GlowCraft
#else
#define SNIFF_SLOTS 16
#endif
static CanSniffSlot s_sniff[SNIFF_SLOTS];

// Starter labels for the Evo-X discovery build. Mitsubishi Evo X (CZ4A) OEM
// bus @500 kbit; IDs below are community reverse-engineered (ECUMaster ADU app
// note, EvolutionM / AutosportLabs captures, EvoScan) and are UNVERIFIED on this
// specific car — the trailing '?' is a reminder to confirm by watching the byte
// move on the sniffer. NOTE: SST clutch/trans temps are NOT here because they are
// request/response PIDs (EvoScan CAN28/CAN33), not broadcast — they won't appear
// in the sniffer at all without an active poller (that's the real finding to prove).
#ifdef EVO_SNIFFER
struct EvoLabel { uint16_t id; const char* name; };
static const EvoLabel EVO_LABELS[] = {
  { 0x308, "RPM?" },
  { 0x210, "Throttle/TPS?" },
  { 0x212, "Idle RPM target?" },
  { 0x380, "Brake/Clutch sw?" },
  { 0x415, "A/C switch?" },
  { 0x608, "Coolant/ECT?" },
  // Semantics known but carrying-ID not yet confirmed (find by watching sniffer):
  //   gear: 0=Park 8=Reverse 16=Neutral 32=Drive
  //   gearbox mode: 1=S-Sport 2=Sport 3=Normal ; diff mode: 1=Tarmac 2=Gravel 3=Snow
};
const char* can_label(uint16_t id) {
  for (auto& l : EVO_LABELS) if (l.id == id) return l.name;
  return nullptr;
}
#else
const char* can_label(uint16_t) { return nullptr; }
#endif

// Record a frame that the Haltech registry didn't claim. One slot per distinct
// ID; when full, the least-recently-seen slot is recycled.
static void sniff_capture(const twai_message_t* m) {
  uint16_t id = (uint16_t)m->identifier;
  int slot = -1, oldest = 0;
  uint32_t oldest_ms = UINT32_MAX;
  for (int i = 0; i < SNIFF_SLOTS; i++) {
    if (s_sniff[i].count && s_sniff[i].id == id) { slot = i; break; }  // existing
    if (s_sniff[i].count == 0 && slot < 0) slot = i;                   // first free
    if (s_sniff[i].last_ms < oldest_ms) { oldest_ms = s_sniff[i].last_ms; oldest = i; }
  }
  if (slot < 0) slot = oldest;                 // table full -> recycle oldest
  CanSniffSlot* s = &s_sniff[slot];
  if (s->id != id) { s->id = id; s->count = 0; }
  s->dlc = m->data_length_code;
  s->ext = m->extd;
  for (int i = 0; i < 8; i++) s->data[i] = (i < m->data_length_code) ? m->data[i] : 0;
  s->last_ms = millis();
  s->count++;
}

const CanSniffSlot* cansniff_table(int* count) {
  if (count) *count = SNIFF_SLOTS;
  return s_sniff;
}

// ---------------- decode task ----------------
void process_can_queue_task(void *arg) {
  build_index();
  twai_message_t message;
  while (1) {
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
#ifdef EVO_SNIFFER
      // Foreign bus (Evo X @500k): the Haltech registry is meaningless here and
      // would mis-claim any overlapping ID (e.g. 0x380), hiding it. Capture every
      // frame raw so nothing is lost during discovery.
      sniff_capture(&message);
#else
      const IdIndex* idx = find_id((uint16_t)message.identifier);
      if (idx) {
        uint32_t now = millis();
        for (int i = idx->first; i < idx->first + idx->count; i++) {
          float v = extract_channel(HALTECH_CHANNELS[i], message.data, message.data_length_code);
          if (!isnan(v)) { s_val[i] = v; s_seen[i] = now; }
        }
        if (message.identifier == 0x3E4) hook_3e4_trimpot(&message);
      } else {
        // GlowCraft strip-status frames (0x500+) — decoded in their own module.
        glowcraft_decode(&message);
        // Also latch every non-Haltech frame for the debug sniffer view.
        sniff_capture(&message);
      }
#endif // EVO_SNIFFER
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// ---------------- test mode ----------------
// Synthetic sweeps in CANONICAL units for the channels the default modes +
// common secondaries use, so the bench behaves like a running engine.
static void inject(uint16_t key, float v) {
  int i = chan_index_from_key(key);
  if (i >= 0) { s_val[i] = v; s_seen[i] = millis(); }
}

void haltech_test_inject() {
  static float t = 0; t += 0.05f;
  uint32_t rpm = (uint32_t)(900 + (sinf(t * 0.4f) + 1) * 3300);
  inject(CHAN_KEY(0x360, 0), rpm);                                       // RPM
  inject(CHAN_KEY(0x360, 2), -100 + (sinf(t) + 1) * 155);                // MAP gauge kPa (~ -15..30 psi)
  inject(CHAN_KEY(0x360, 4), (sinf(t * 0.6f) + 1) * 50);                 // TPS %
  inject(CHAN_KEY(0x368, 0), 0.75f + (sinf(t * 0.5f) + 1) * 0.25f);      // WB1 lambda (~11..22 AFR)
  inject(CHAN_KEY(0x361, 2), 70 + (sinf(t * 0.7f) + 1) * 310);           // Oil press gauge kPa (~10..100 psi)
  inject(CHAN_KEY(0x361, 0), 250 + (sinf(t * 0.4f) + 1) * 45);           // Fuel press kPa
  inject(CHAN_KEY(0x3E0, 0), 323 + (sinf(t * 0.3f) + 1) * 35);           // Coolant K (~50..120 C)
  inject(CHAN_KEY(0x3E0, 2), 298 + (sinf(t * 0.2f) + 1) * 22);           // Air temp K
  inject(CHAN_KEY(0x3E0, 4), 308 + (sinf(t * 0.18f) + 1) * 10);          // Fuel temp K
  inject(CHAN_KEY(0x3E0, 6), 353 + (sinf(t * 0.15f) + 1) * 20);          // Oil temp K
  inject(CHAN_KEY(0x370, 0), (sinf(t * 0.1f) + 1) * 80);                 // Speed km/h
  inject(CHAN_KEY(0x372, 0), 13.2f + sinf(t * 0.9f) * 0.6f);             // Battery V
  inject(CHAN_KEY(0x470, 7), (float)((int)((sinf(t * 0.08f) + 1) * 3) + 1)); // Gear
}
