#include "Arduino.h"
#include "GlowCraft_Driver.h"

// Strip registry — the single source of truth for layout & CAN mapping.
// can_id is filled in glowcraft_init() as GLOWCRAFT_CAN_BASE + index.
GlowCraftStrip glowcraft_strips[GC_STRIP_COUNT] = {
  // name              pixels installed
  { "Front",            30,   true  },
  { "Front Badge",       4,   true  },
  { "Left Headlight",    2,   true  },
  { "Right Headlight",   2,   true  },
  { "Left",             50,   true  },
  { "Right",            50,   true  },
  { "Rear",             30,   true  },
  { "Front Plate",       0,   false },  // planned, pixel count TBD
};

void glowcraft_init(void) {
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    GlowCraftStrip* s = &glowcraft_strips[i];
    s->can_id       = GLOWCRAFT_CAN_BASE + i;
    s->r = s->g = s->b = 0;
    s->brightness   = 0;
    s->state        = GC_STATE_OFF;
    s->last_seen_ms = 0;  // never seen
  }
}

bool glowcraft_decode(const twai_message_t* msg) {
  uint32_t id = msg->identifier;
  if (id < GLOWCRAFT_CAN_BASE || id >= (GLOWCRAFT_CAN_BASE + GC_STRIP_COUNT)) {
    return false;  // not one of ours
  }
  int idx = id - GLOWCRAFT_CAN_BASE;
  GlowCraftStrip* s = &glowcraft_strips[idx];
  s->r          = msg->data[0];
  s->g          = msg->data[1];
  s->b          = msg->data[2];
  s->brightness = msg->data[3];
  s->state      = msg->data[4];
  s->last_seen_ms = millis();
  return true;
}

bool glowcraft_strip_online(int idx, uint32_t now_ms) {
  if (idx < 0 || idx >= GC_STRIP_COUNT) return false;
  uint32_t seen = glowcraft_strips[idx].last_seen_ms;
  if (seen == 0) return false;  // never seen
  return (now_ms - seen) < GLOWCRAFT_OFFLINE_TIMEOUT_MS;
}

uint32_t glowcraft_strip_color(int idx, uint32_t now_ms) {
  if (!glowcraft_strip_online(idx, now_ms)) return 0;
  const GlowCraftStrip* s = &glowcraft_strips[idx];
  // Apply master brightness scale (0-255) to each channel.
  uint16_t scale = s->brightness;
  uint8_t r = (uint16_t)s->r * scale / 255;
  uint8_t g = (uint16_t)s->g * scale / 255;
  uint8_t b = (uint16_t)s->b * scale / 255;
  return ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}
