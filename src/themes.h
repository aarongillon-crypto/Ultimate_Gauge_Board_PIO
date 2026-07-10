// Theme slots + cross-task theme staging.
//
// The live colour globals are owned by loopTask. Theme changes arriving on
// other tasks (ESP-NOW callback on core 0, ProcCAN trimpot decode) are STAGED
// here and consumed by themes_process_pending() in loop() — they never write
// the colour globals directly, so LVGL can never render a half-applied theme.
#pragma once
#include "app_state.h"

// Human-readable slot names (NVS t{n}_nm; defaults Custom/Street/Sport/Race).
extern String theme_names[THEME_SLOTS];

// Copy a stored slot into the live colour globals (used by the renderer).
void theme_to_globals(uint8_t i);
// Capture the current live colour globals back into a slot (after a web edit).
void globals_to_theme(uint8_t i);
// Persist one slot to NVS (via config_store).
void persist_theme(uint8_t i);
// Load all four slots from NVS; slot 0 defaults to the already-loaded live
// (legacy) theme so an existing single theme migrates seamlessly.
void load_all_themes();

// --- Cross-task staging -----------------------------------------------------
enum PendingThemeOp : uint8_t {
  PT_NONE = 0,
  PT_DYNAMIC_COLORS,   // ESP-NOW type 3: text/low/mid/high
  PT_UI_COLORS,        // ESP-NOW type 7: bg/mode-label/link-icon/needle/peak
  PT_GRADIENT,         // ESP-NOW type 8: stops 2/3, stop1, packed type|stops|angle
  PT_ACTIVATE_SLOT,    // trimpot: make slot N live (not persisted — matches rotary semantics)
};
typedef struct {
  PendingThemeOp op;
  uint8_t slot;                     // PT_ACTIVATE_SLOT
  uint32_t c1, c2, c3, c4, c5;      // colour payloads
  uint8_t gt, gs; uint16_t ga;      // gradient payloads
} PendingTheme;

// Stage a theme change from ANY task (single-slot; the latest write wins).
void theme_stage(const PendingTheme* p);
// Consume the staged change in loop(): applies to globals/slots, persists via
// config_store, and sets flag_theme_update. No-op when nothing is staged.
void themes_process_pending();
