#include "themes.h"
#include "config_store.h"

// Distinct factory palettes for slots 1-3 (slot 0 inherits the existing theme).
static const GaugeTheme THEME_DEFAULTS[THEME_SLOTS] = {
  // slot 0 — placeholder; overwritten by the live/legacy theme at load time (solid bg)
  {0xFFD700,0x2196F3,0x4CAF50,0xF44336, 0x000000,0x969696,0x00C851,0xFF6600,0xFFFFFF, 0x000000,0x000000, 0,2,0},
  // slot 1 — "Street" cool blue: vertical fade to black
  {0xFFFFFF,0x2196F3,0x4CAF50,0xF44336, 0x001830,0x6699BB,0x00C851,0x33AAFF,0xFFFFFF, 0x000000,0x000000, 1,2,0},
  // slot 2 — "Sport" amber: radial glow from centre
  {0xFFD700,0x00C8FF,0xFFC107,0xFF3B30, 0x1A1000,0xAA8844,0xFFAA00,0xFF8A00,0xFFFFFF, 0x000000,0x000000, 4,2,0},
  // slot 3 — "Race" red: 3-stop linear at 45°
  {0xFFFFFF,0x00E676,0xFFEB3B,0xFF1744, 0x200000,0xBB5555,0xFF3B30,0xFF1744,0xFFFFFF, 0x080000,0x000000, 3,3,45},
};

String theme_names[THEME_SLOTS];
static const char* THEME_NAME_DEFAULTS[THEME_SLOTS] = { "Custom", "Street", "Sport", "Race" };

void theme_to_globals(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  const GaugeTheme &t = themes[i];
  text_color = t.text; color_low = t.low; color_mid = t.mid; color_high = t.high;
  color_background = t.background; color_mode_label = t.mode_label;
  color_link_icon = t.link_icon; needle_color = t.needle; color_peak = t.peak;
  color_background2 = t.bg_grad2; color_background3 = t.bg_grad3;
  bg_grad_type = t.bg_grad_type; bg_grad_stops = t.bg_grad_stops; bg_grad_angle = t.bg_grad_angle;
}

void globals_to_theme(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  GaugeTheme &t = themes[i];
  t.text = text_color; t.low = color_low; t.mid = color_mid; t.high = color_high;
  t.background = color_background; t.mode_label = color_mode_label;
  t.link_icon = color_link_icon; t.needle = needle_color; t.peak = color_peak;
  t.bg_grad2 = color_background2; t.bg_grad3 = color_background3;
  t.bg_grad_type = bg_grad_type; t.bg_grad_stops = bg_grad_stops; t.bg_grad_angle = bg_grad_angle;
}

void persist_theme(uint8_t i) {
  if (i >= THEME_SLOTS) return;
  cfg_persist_theme_slot(i, themes[i]);
}

void load_all_themes() {
  for (uint8_t i = 0; i < THEME_SLOTS; i++) {
    GaugeTheme d = THEME_DEFAULTS[i];
    if (i == 0) {  // slot 0 default = the current live (legacy) theme
      d.text = text_color; d.low = color_low; d.mid = color_mid; d.high = color_high;
      d.background = color_background; d.mode_label = color_mode_label;
      d.link_icon = color_link_icon; d.needle = needle_color; d.peak = color_peak;
      d.bg_grad2 = color_background2; d.bg_grad3 = color_background3;
      d.bg_grad_type = bg_grad_type; d.bg_grad_stops = bg_grad_stops; d.bg_grad_angle = bg_grad_angle;
    }
    cfg_load_theme_slot(i, d, &themes[i]);
    theme_names[i] = cfg_load_theme_name(i, THEME_NAME_DEFAULTS[i]);
  }
}

// --- Cross-task staging -----------------------------------------------------
static PendingTheme s_pending = {};
static volatile bool s_pending_valid = false;
static portMUX_TYPE s_theme_mux = portMUX_INITIALIZER_UNLOCKED;

void theme_stage(const PendingTheme* p) {
  taskENTER_CRITICAL(&s_theme_mux);
  s_pending = *p;
  s_pending_valid = true;
  taskEXIT_CRITICAL(&s_theme_mux);
}

static bool theme_take_pending(PendingTheme* out) {
  if (!s_pending_valid) return false;   // cheap pre-check, loop polls every frame
  taskENTER_CRITICAL(&s_theme_mux);
  *out = s_pending;
  s_pending_valid = false;
  taskEXIT_CRITICAL(&s_theme_mux);
  return true;
}

void themes_process_pending() {
  PendingTheme p;
  if (!theme_take_pending(&p)) return;

  switch (p.op) {
    case PT_DYNAMIC_COLORS:   // ESP-NOW type 3 (was: direct global writes in the callback)
      text_color = p.c1; color_low = p.c2; color_mid = p.c3; color_high = p.c4;
      cfg_put_uint("ct", text_color); cfg_put_uint("cl", color_low);
      cfg_put_uint("cm", color_mid);  cfg_put_uint("ch", color_high);
      globals_to_theme(active_theme); persist_theme(active_theme);
      break;
    case PT_UI_COLORS:        // ESP-NOW type 7
      color_background = p.c1; color_mode_label = p.c2; color_link_icon = p.c3;
      needle_color = p.c4; color_peak = p.c5;
      cfg_put_uint("cbg", color_background); cfg_put_uint("cml", color_mode_label);
      cfg_put_uint("cli", color_link_icon);  cfg_put_uint("cn", needle_color);
      cfg_put_uint("cp", color_peak);
      globals_to_theme(active_theme); persist_theme(active_theme);
      break;
    case PT_GRADIENT:         // ESP-NOW type 8
      color_background2 = p.c1; color_background3 = p.c2; color_background = p.c3;
      bg_grad_type = p.gt; bg_grad_stops = p.gs; bg_grad_angle = p.ga;
      cfg_put_uint("cbg", color_background); cfg_put_uint("cbg2", color_background2);
      cfg_put_uint("cbg3", color_background3);
      cfg_put_uchar("cgt", bg_grad_type); cfg_put_uchar("cgs", bg_grad_stops);
      cfg_put_ushort("cga", bg_grad_angle);
      globals_to_theme(active_theme); persist_theme(active_theme);
      break;
    case PT_ACTIVATE_SLOT:    // trimpot rotary — live only, not persisted (matches old behavior)
      if (p.slot < THEME_SLOTS) {
        active_theme = p.slot;
        theme_to_globals(active_theme);
      }
      break;
    case PT_FULL_THEME:       // fleet v2 THEME_FULL: store whole slot (+optional activate)
      if (p.slot < THEME_SLOTS) {
        themes[p.slot] = p.full;
        theme_names[p.slot] = String(p.fname);
        persist_theme(p.slot);
        cfg_put_theme_name(p.slot, theme_names[p.slot]);
        if (p.apply) {
          active_theme = p.slot;
          cfg_put_uint("atheme", active_theme);
        }
        if (p.slot == active_theme) theme_to_globals(active_theme);
        else return;          // stored a non-active slot — nothing to repaint
      }
      break;
    default: return;
  }
  flag_theme_update = true;
}
