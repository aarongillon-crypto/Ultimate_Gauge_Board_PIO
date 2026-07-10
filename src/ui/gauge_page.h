// The main gauge page: build, style application, and the per-frame update path.
#pragma once
#include "../app_state.h"

// Full build of the gauge screen's objects (boot / rare rebuild only).
void load_current_style();
// Apply the current theme's colours/gradient/font to the EXISTING objects
// without tearing the screen down (every theme change routes here).
void apply_theme_colors();
// Per-frame update: reads a HaltechData snapshot, smooths, draws.
void update_gauge_master();
// Load the screen matching current_page (gauge or GlowCraft).
void apply_page();
