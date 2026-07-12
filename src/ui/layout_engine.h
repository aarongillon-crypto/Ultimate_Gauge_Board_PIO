// Runtime layout engine (Layout_Engine_Spec.md §6) — parses a Layout Schema
// v1 JSON document and renders it via LVGL on the gauge screen, replacing the
// built-in face while a valid layout is stored + enabled.
//
// Spike scope (spec M1): all 7 schema-v1 element types (they map onto the
// already-enabled lv_obj/lv_label/lv_line primitives), single rendered page
// (start_page), hex colours + theme tokens (resolved at build; rebuilt on
// theme change), solid/theme/gradient backgrounds. Deferred: visible_if,
// stale_ms, page switching, images/custom fonts.
//
// Ownership: ALL functions here must be called from loopTask.
#pragma once
#include "../app_state.h"

// Validate a candidate document (without applying). Returns true when
// renderable; on failure writes a short reason into err (for the 400 body).
bool layout_validate(const String& json, String& err);

// (Re)load the stored layout from layout_store and build it onto the gauge
// screen. Returns true if a layout is now active; false = built-in face
// (caller should then run load_current_style()).
bool layout_engine_load();

// Tear down the layout scene (before reverting to the built-in face).
void layout_engine_unload();

bool layout_engine_active();
const char* layout_engine_name();   // meta.name or "" — for /api/state
int layout_engine_page();           // active page index
int layout_engine_page_count();     // pages in the active layout

// Switch the active page (rebuilds the scene). False if no layout / bad index.
// Call from loopTask only.
bool layout_engine_set_page(int idx);

// Per-frame update of bound elements (call instead of update_gauge_master()
// while active). Applies behavior.smoothing to bound values.
void layout_engine_update();

// Re-resolve theme-token colours + repaint (call on flag_theme_update).
void layout_engine_theme_changed();
