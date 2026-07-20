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

// -----------------------------------------------------------------------------
// GlowCraft signals frame (GlowCraft -> gauge digital inputs)
// -----------------------------------------------------------------------------
// The GlowCraft's "Send CAN Status" broadcasts vehicle/lighting state so the
// gauge can consume signals the Haltech bus may not carry (e.g. park light).
// Unlike the strip frames, THIS layout is fixed by GlowCraft firmware — we
// only choose the CAN ID. The layout below was reverse-engineered on the bench
// with the CAN sniffer (see the /api/cansniff web view), not defined by us.
//
// Observed 0x520 payload (ID = the "Send CAN Status" address; we set it to
// 0x520), bit numbering from LSB (bit0 = 0x01):
//   byte0: b7 & b6 are ALWAYS set  -> "status valid / online" (heartbeat-ish)
//          b5 (0x20) = Park light / headlight-show active   <-- consumed
//          idle = 0xC0, park on = 0xE0
//   byte1: b5 (0x20) seen toggling on its own (idle/animation/timeout state);
//          meaning TBD, retained in glowcraft_signals.vehicle but not consumed.
//   byte2..7: 0 in all captures.
//
// Scope today: only the Park bit is consumed (screen dimming). Fail-safe: a
// stale frame (older than GLOWCRAFT_OFFLINE_TIMEOUT_MS) reads park as false.
// If the GlowCraft firmware's bit layout changes, re-check with the sniffer and
// update GC_SIG_PARK / GLOWCRAFT_SIGNAL_CAN below.
#define GLOWCRAFT_SIGNAL_CAN          (GLOWCRAFT_CAN_BASE + 0x20)   // 0x520

#define GC_SIG_PARK  0x20   // byte0 b5 (0xC0 idle -> 0xE0 when park active)

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

// Digital-signals state, written by glowcraft_decode() from the 0x520 frame.
typedef struct {
  uint8_t  lighting;      // byte0 bitfield (GC_SIG_PARK is b5, 0x20)
  uint8_t  vehicle;       // byte1 bitfield (reserved for later use)
  uint32_t last_seen_ms;  // millis() of last 0x520 frame; 0 = never seen
} GlowCraftSignals;
extern GlowCraftSignals glowcraft_signals;

// Reset the registry's live state. Call once at startup.
void glowcraft_init();

// Decode a CAN frame. Returns true if it was a GlowCraft strip-status frame and
// was consumed (live state updated); false if the ID is not ours.
bool glowcraft_decode(const twai_message_t* msg);

// True if the strip has reported within GLOWCRAFT_OFFLINE_TIMEOUT_MS.
bool glowcraft_strip_online(int idx, uint32_t now_ms);

// True if the Park/position-light signal (0x510 b0) is on AND the signals frame
// is fresh. Fail-safe false when stale or never seen.
bool glowcraft_park_active(uint32_t now_ms);

// Packed 0x00RRGGBB for the strip with brightness applied, or 0 if offline.
uint32_t glowcraft_strip_color(int idx, uint32_t now_ms);
