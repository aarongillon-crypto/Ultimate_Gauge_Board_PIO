#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "driver/twai.h"

// =============================================================================
// GlowCraft strip-status CAN protocol  (gauge-side definition)
// =============================================================================
// We define this protocol; the GlowCraft GC01-P is then configured to broadcast
// matching frames. Decode logic is isolated here so the on-wire format can be
// changed without touching the display layer.
//
// One CAN ID per strip, contiguous from GLOWCRAFT_CAN_BASE:
//     can_id = GLOWCRAFT_CAN_BASE + GlowCraftStripId
//
// Payload (8 bytes, big-endian convention to match Haltech bus):
//     [0] R           0-255
//     [1] G           0-255
//     [2] B           0-255
//     [3] brightness  0-255  (master scale applied to RGB for display)
//     [4] state       see GlowCraftState
//     [5..7] reserved (0)
//
// A representative colour per strip is sent rather than per-pixel data — full
// per-pixel state for 160+ pixels is not viable over CAN, and the gauge page
// only needs a static representation of each strip's current state.
//
// Adding strips later (e.g. rear plate): append before GC_STRIP_COUNT so the
// existing CAN IDs stay stable, then add a row to glowcraft_strips[].
// =============================================================================

#define GLOWCRAFT_CAN_BASE            0x500
#define GLOWCRAFT_OFFLINE_TIMEOUT_MS  2000   // strip greyed out if no frame within this

enum GlowCraftState : uint8_t {
  GC_STATE_OFF       = 0,   // strip idle / dark
  GC_STATE_SOLID     = 1,   // solid colour
  GC_STATE_ANIMATING = 2,   // animation running (colour = representative frame)
};

// Strip identity = offset from GLOWCRAFT_CAN_BASE. Order is the CAN ID order.
enum GlowCraftStripId {
  GC_STRIP_FRONT = 0,       // 0x500
  GC_STRIP_FRONT_BADGE,     // 0x501  lit Mitsubishi diamond in grille
  GC_STRIP_LEFT_HEADLIGHT,  // 0x502
  GC_STRIP_RIGHT_HEADLIGHT, // 0x503
  GC_STRIP_LEFT,            // 0x504
  GC_STRIP_RIGHT,           // 0x505
  GC_STRIP_REAR,            // 0x506
  GC_STRIP_FRONT_PLATE,     // 0x507  planned — not installed, pixel count TBD
  GC_STRIP_COUNT
};

typedef struct {
  const char* name;
  uint16_t    pixel_count;   // 0 = not installed / TBD
  bool        installed;
  uint32_t    can_id;
  // --- live state, written by glowcraft_decode() ---
  uint8_t     r, g, b;
  uint8_t     brightness;
  uint8_t     state;         // GlowCraftState
  uint32_t    last_seen_ms;  // millis() of last frame; 0 = never seen
} GlowCraftStrip;

extern GlowCraftStrip glowcraft_strips[GC_STRIP_COUNT];

// Reset the registry's live state. Call once at startup.
void glowcraft_init();

// Decode a CAN frame. Returns true if it was a GlowCraft strip-status frame and
// was consumed (live state updated); false if the ID is not ours.
bool glowcraft_decode(const twai_message_t* msg);

// True if the strip has reported within GLOWCRAFT_OFFLINE_TIMEOUT_MS.
bool glowcraft_strip_online(int idx, uint32_t now_ms);

// Packed 0x00RRGGBB for the strip with brightness applied, or 0 if offline.
uint32_t glowcraft_strip_color(int idx, uint32_t now_ms);
