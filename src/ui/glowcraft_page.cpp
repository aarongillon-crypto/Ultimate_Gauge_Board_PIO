#include "glowcraft_page.h"
#include "render_shared.h"
#include "GlowCraft_Driver.h"
#include <math.h>

// --- GLOWCRAFT LED PAGE ---
// Top-down Evo silhouette (front at top) with each LED strip drawn as a
// coloured rectangle in its physical position. Layout is a table parallel to
// the strip registry (glowcraft_strips[]), so adding a strip is a one-row edit
// here plus one in the driver. Coordinates are absolute on the 480x480 screen.
struct StripLayout { int16_t x, y, w, h, radius; };
static const StripLayout strip_layout[GC_STRIP_COUNT] = {
  /* GC_STRIP_FRONT           */ { 172,  86, 136, 16, 6 },  // front bar (top)
  /* GC_STRIP_FRONT_BADGE     */ { 222, 110,  36, 26, 6 },  // grille diamond
  /* GC_STRIP_LEFT_HEADLIGHT  */ { 158, 112,  44, 22, 6 },
  /* GC_STRIP_RIGHT_HEADLIGHT */ { 278, 112,  44, 22, 6 },
  /* GC_STRIP_LEFT            */ { 152, 176,  16, 170, 6 }, // left flank (vertical)
  /* GC_STRIP_RIGHT           */ { 312, 176,  16, 170, 6 }, // right flank (vertical)
  /* GC_STRIP_REAR            */ { 172, 398, 136, 16, 6 },  // rear bar (bottom)
  /* GC_STRIP_FRONT_PLATE     */ { 205, 146,  70, 14, 4 },  // placeholder (not installed)
};

static lv_obj_t *strip_widget[GC_STRIP_COUNT] = { nullptr };
static const uint32_t GC_DIM_COLOR = 0x202020;  // strip off / offline

// Builds the GlowCraft screen once. update_glowcraft_page() refreshes the
// strip colours each frame while this page is active.
void build_glowcraft_page() {
  glowcraft_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(glowcraft_scr, lv_color_black(), 0);
  lv_obj_set_style_pad_all(glowcraft_scr, 0, 0);
  lv_obj_clear_flag(glowcraft_scr, LV_OBJ_FLAG_SCROLLABLE);

  // Car body outline behind the strips (drawn first = lowest z-order).
  lv_obj_t *body = lv_obj_create(glowcraft_scr);
  lv_obj_remove_style_all(body);
  lv_obj_set_pos(body, 150, 70);
  lv_obj_set_size(body, 180, 350);
  lv_obj_set_style_radius(body, 40, 0);
  lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(body, 2, 0);
  lv_obj_set_style_border_color(body, lv_color_hex(0x404040), 0);
  lv_obj_clear_flag(body, LV_OBJ_FLAG_SCROLLABLE);

  lv_obj_t *title = lv_label_create(glowcraft_scr);
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_set_style_text_color(title, lv_color_hex(0x888888), 0);
  lv_label_set_text(title, "GlowCraft");
  lv_obj_align(title, LV_ALIGN_CENTER, 0, 0);

  // Strip rectangles.
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    const StripLayout* L = &strip_layout[i];
    lv_obj_t* w = lv_obj_create(glowcraft_scr);
    lv_obj_remove_style_all(w);
    lv_obj_set_pos(w, L->x, L->y);
    lv_obj_set_size(w, L->w, L->h);
    lv_obj_set_style_radius(w, L->radius, 0);
    lv_obj_clear_flag(w, LV_OBJ_FLAG_SCROLLABLE);
    if (glowcraft_strips[i].installed) {
      lv_obj_set_style_bg_opa(w, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(w, lv_color_hex(GC_DIM_COLOR), 0);
    } else {
      // Planned-but-not-installed strip: dashed-look outline, no fill.
      lv_obj_set_style_bg_opa(w, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(w, 1, 0);
      lv_obj_set_style_border_color(w, lv_color_hex(0x505050), 0);
    }
    strip_widget[i] = w;
  }
}

// Bench test: drive a slow rolling rainbow into the strip registry as if real
// 0x500+ frames were arriving, so the page can be verified without a live
// GlowCraft. Gated behind test_mode_enabled by the caller.
void glowcraft_test_inject() {
  uint32_t now = millis();
  float phase = now * 0.001f;  // slow cycle
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    if (!glowcraft_strips[i].installed) continue;
    float a = phase + i * 0.8f;  // per-strip hue offset
    glowcraft_strips[i].r = (uint8_t)((sinf(a)          * 0.5f + 0.5f) * 255);
    glowcraft_strips[i].g = (uint8_t)((sinf(a + 2.094f) * 0.5f + 0.5f) * 255);  // +120 deg
    glowcraft_strips[i].b = (uint8_t)((sinf(a + 4.188f) * 0.5f + 0.5f) * 255);  // +240 deg
    glowcraft_strips[i].brightness   = 255;
    glowcraft_strips[i].state        = GC_STATE_ANIMATING;
    glowcraft_strips[i].last_seen_ms = now;  // mark online
  }
}

// Refresh strip widget colours from the live registry. Colours are cached so a
// widget is only invalidated when its colour actually changes (keeps the page
// cheap to render — same discipline as the gauge's needle/label caching).
void update_glowcraft_page() {
  static uint32_t prev_col[GC_STRIP_COUNT] = { 0 };
  static bool init = false;
  if (!init) { for (int i = 0; i < GC_STRIP_COUNT; i++) prev_col[i] = 0xFFFFFFFFu; init = true; }

  uint32_t now = millis();
  for (int i = 0; i < GC_STRIP_COUNT; i++) {
    if (!strip_widget[i] || !glowcraft_strips[i].installed) continue;  // placeholders untouched
    uint32_t col = glowcraft_strip_color(i, now);
    uint32_t shown = col ? col : GC_DIM_COLOR;   // dim when off/offline
    if (shown == prev_col[i]) continue;          // unchanged — skip invalidation
    prev_col[i] = shown;
    lv_obj_set_style_bg_color(strip_widget[i], lv_color_hex(shown), 0);
  }
}
