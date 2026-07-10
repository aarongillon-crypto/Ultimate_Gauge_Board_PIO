// GlowCraft LED status page: top-down car silhouette with live strip colours.
#pragma once
#include "../app_state.h"

void build_glowcraft_page();     // build once at boot
void update_glowcraft_page();    // per-frame colour refresh (dirty-cached)
void glowcraft_test_inject();    // bench rainbow into the strip registry
