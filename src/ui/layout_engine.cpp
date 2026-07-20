#include "layout_engine.h"
#include "render_shared.h"
#include "../layout_store.h"
#include "../haltech_channels.h"
#include "../haltech_decode.h"
#include "../themes.h"
#include "icons_gen.h"
#include "Display_ST7701.h"   // lcd_set_pclk / LCD_PCLK_* (reload-burst clock drop)
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>

// Telltale icon lookup (table generated in icons_gen.h via X-macro).
static const lv_image_dsc_t* icon_by_name(const char* n) {
  #define X(s, sym) if (strcmp(n, s) == 0) return &sym;
  ICON_TABLE_ENTRIES
  #undef X
  return nullptr;
}

// ---------------- fonts (spec §5: fixed v1 set, referenced by name) ----------------
LV_FONT_DECLARE(dseg14_96);
LV_FONT_DECLARE(dseg14_120);
LV_FONT_DECLARE(firamono_96);
LV_FONT_DECLARE(firamono_120);

static const struct { const char* name; const lv_font_t* font; } FONT_TABLE[] = {
  { "dseg14_120",    &dseg14_120 },
  { "dseg14_96",     &dseg14_96 },
  { "firamono_120",  &firamono_120 },
  { "firamono_96",   &firamono_96 },
  { "montserrat_28", &lv_font_montserrat_28 },
  { "montserrat_20", &lv_font_montserrat_20 },
  { "montserrat_14", &lv_font_montserrat_14 },
};
static const lv_font_t* font_by_name(const char* n) {
  if (!n) return nullptr;
  for (auto& e : FONT_TABLE) if (strcmp(e.name, n) == 0) return e.font;
  return nullptr;
}

// ---------------- runtime model ----------------
// Caps per spec §10. Static storage (~9 KB bss) — small vs the 59 KB in use.
#define LE_MAX_ELEMENTS 64
#define LE_STR_MAX 32

enum LeType : uint8_t { LE_TEXT, LE_NUMERIC, LE_NEEDLE, LE_RING, LE_BAR, LE_SHAPE, LE_WARNING, LE_SHIFT,
                        LE_SCALE, LE_LED, LE_IMAGE, LE_ALERT };
enum LeShape : uint8_t { LE_RECT, LE_CIRCLE, LE_LINE };

// Colour: hex value, or a theme-token role re-resolved on theme change.
struct LeColor { uint32_t hex; int8_t role; };   // role -1 = literal hex

struct LeElement {
  LeType   type;
  int16_t  chan_idx;          // resolved registry index, -1 = unbound
  float    min, max, z1, z2;
  int16_t  x, y, cx, cy;
  int16_t  r0, r1, r, w, h;
  int16_t  start_deg, sweep_deg;
  uint8_t  width, radius, decimals;
  uint8_t  align;             // 0=left 1=center 2=right
  bool     unit, off_hidden, horiz;
  LeColor  col, low, mid, high, track, stroke;
  bool     has_fill, has_stroke;
  uint8_t  stroke_w;
  const lv_font_t* font;
  char     str[LE_STR_MAX];   // text / warning on_str / numeric prefix
  // visibility gates (spec §3.3)
  int16_t  vis_chan_idx;      // visible_if channel, -1 = none
  uint8_t  vis_op;            // OP_*
  float    vis_value;
  uint16_t stale_ms;          // hide when bound channel older than this (0 = off)
  uint8_t  n_pts;             // shape:"line" — point count (0 = rect/circle)
  int8_t   prev_shown;        // gate cache: -1 unknown, 0 hidden, 1 shown
  // extensions (Phase 1): shiftlights / bar origin / peak needle / pulse
  uint8_t  segs;              // shiftlights: segment count (0 = n/a)
  int16_t  gap;               // shiftlights: inter-segment gap px
  bool     flash_max;         // shiftlights: blink all at/above max
  bool     has_origin;        // bar: grow fill from origin instead of min
  float    origin;            // bar: origin value
  bool     peak_src;          // needle: park at session max instead of live
  float    peak;              // needle: peak accumulator
  bool     pulse;             // any: opacity blink via lv_anim
  // Phase 2 widgets: scale / led / image
  uint8_t  sc_major, sc_minor;// scale: major-tick count, minor ticks per major
  int16_t  sc_maj_len, sc_min_len;
  bool     sc_labels;         // scale: draw numeric labels on major ticks
  float    sc_from, sc_to;    // scale: section (redline) value range (to<=from = none)
  float    led_on;            // led: on threshold (display units)
  const void* icon;           // image: lv_image_dsc_t*
  // Phase 3 (tier-3, measure-and-cull)
  bool     alert_full;        // alert: fullscreen overlay vs edge ring
  uint8_t  alert_opa;         // alert: fullscreen overlay opacity
  bool     img_needle;        // needle: rotated bitmap sprite instead of a line
  bool     has_shadow;        // any: blur shadow (heavy — measured)
  int16_t  shadow_w, shadow_spread, shadow_ox, shadow_oy;
  uint8_t  shadow_opa;
  LeColor  shadow_col;
  // LVGL handles + dirty caches
  lv_obj_t* obj;
  lv_obj_t* obj2;
  float    disp;              // smoothed displayed value
  char     prev_txt[LE_STR_MAX];
  uint32_t prev_col;
  int32_t  prev_a, prev_b;    // needle endpoints hash / bar fill px / visibility
};

enum { OP_GT, OP_LT, OP_GE, OP_LE, OP_EQ, OP_NE, OP_TRUTHY };

static LeElement s_el[LE_MAX_ELEMENTS];
static int s_count = 0;
static bool s_active = false;
static char s_name[24] = "";
static int s_page_count = 1;   // pages in the active layout
static int s_active_page = 0;  // index of the built page
static char s_bg_json[160] = "";       // background spec kept for theme rebuilds
static lv_grad_dsc_t s_bg_grad;        // style keeps a pointer — must persist
// Persistent point storage for shape:"line" (lv_line keeps a pointer, doesn't
// copy). Raw parsed points; converted to lv_point_precise_t at build.
#define LE_MAX_LINE_PTS 8
static int16_t s_pts[LE_MAX_ELEMENTS][LE_MAX_LINE_PTS][2];
static lv_point_precise_t s_line_lv[LE_MAX_ELEMENTS][LE_MAX_LINE_PTS];
// Per-scale section style (LVGL keeps the pointer, so it must persist). One slot
// per element index; reset before reuse so a page reload doesn't leak props.
static lv_style_t s_sec_style[LE_MAX_ELEMENTS];
static bool s_sec_used[LE_MAX_ELEMENTS];

// ---------------- colour helpers ----------------
static const char* TOKEN_ROLES[] = { "text", "low", "mid", "high", "bg",
                                     "modeLabel", "linkIcon", "needle", "peak" };
static uint32_t role_color(int8_t role) {
  switch (role) {
    case 0: return text_color;      case 1: return color_low;
    case 2: return color_mid;       case 3: return color_high;
    case 4: return color_background;case 5: return color_mode_label;
    case 6: return color_link_icon; case 7: return needle_color;
    case 8: return color_peak;      default: return 0xFFFFFF;
  }
}
static bool parse_color(JsonVariantConst v, LeColor* out) {
  out->role = -1; out->hex = 0xFFFFFF;
  if (v.is<const char*>()) {
    const char* s = v.as<const char*>();
    if (!s || s[0] != '#' || strlen(s) != 7) return false;
    out->hex = strtoul(s + 1, nullptr, 16);
    return true;
  }
  if (v.is<JsonObjectConst>()) {
    JsonObjectConst o = v.as<JsonObjectConst>();
    if (strcmp(o["type"] | "", "theme") != 0) return false;
    const char* role = o["role"] | "";
    for (int i = 0; i < 9; i++) {
      if (strcmp(role, TOKEN_ROLES[i]) == 0) { out->role = (int8_t)i; out->hex = role_color(i); return true; }
    }
    return false;
  }
  return false;
}
static uint32_t col_val(const LeColor& c) { return c.role >= 0 ? role_color(c.role) : c.hex; }

// Forward decl: zone_col is defined in the per-frame section but used by
// build_element (shiftlights colours its segments by threshold at build time).
static uint32_t zone_col(const LeElement& el, float v);

// pulse: infinite opacity blink via LVGL's core animation engine (no per-frame
// work in our update path; a hidden object's anim just sets style, no redraw).
static void anim_opa_cb(void* obj, int32_t v) { lv_obj_set_style_opa((lv_obj_t*)obj, (lv_opa_t)v, 0); }
static void start_pulse(lv_obj_t* o) {
  lv_anim_t a; lv_anim_init(&a);
  lv_anim_set_var(&a, o);
  lv_anim_set_values(&a, LV_OPA_COVER, 40);
  lv_anim_set_duration(&a, 450);
  lv_anim_set_playback_duration(&a, 450);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_set_exec_cb(&a, anim_opa_cb);
  lv_anim_start(&a);
}

// ---------------- parse + validate (spec §9) ----------------
static bool coord_ok(long v) { return v >= -512 && v <= 1023; }

// Top-level validation shared by validate/load/set_page. Reports the page
// count and the index of the start page (by matching start_page against ids).
static bool validate_top(JsonDocument& doc, String& err, int* page_count, int* start_idx) {
  if (strcmp(doc["schema"] | "", "ugb-layout") != 0) { err = "schema must be ugb-layout"; return false; }
  if ((int)(doc["v"] | 0) != 1) { err = "unsupported schema version"; return false; }
  JsonObjectConst canvas = doc["canvas"];
  if ((int)(canvas["w"] | 0) != 480 || (int)(canvas["h"] | 0) != 480 || !(bool)(canvas["round"] | false)) {
    err = "canvas must be 480x480 round"; return false;
  }
  JsonArrayConst pages = doc["pages"];
  if (pages.isNull() || pages.size() < 1 || pages.size() > 4) { err = "need 1..4 pages"; return false; }
  *page_count = pages.size();
  int start_id = doc["start_page"] | 0, i = 0;
  *start_idx = 0;
  for (JsonObjectConst p : pages) { if ((int)(p["id"] | -1) == start_id) { *start_idx = i; break; } i++; }
  return true;
}

// Parse/validate ONE page's elements. When apply, fills s_el/s_count/s_bg_json
// and s_name. Assumes validate_top already passed. err gets a reason on failure.
static bool parse_page_elements(JsonObjectConst page, const char* meta_name, bool apply, String& err) {
  JsonArrayConst els = page["elements"];
  if (els.isNull()) { err = "page has no elements array"; return false; }
  if (els.size() > LE_MAX_ELEMENTS) { err = "too many elements (max 64)"; return false; }

  int n = 0;
  LeElement* out = apply ? s_el : nullptr;

  for (JsonObjectConst e : els) {
    LeElement tmp = {};
    LeElement& el = out ? out[n] : tmp;
    el = {};
    el.chan_idx = -1;
    el.vis_chan_idx = -1;
    el.prev_shown = -1;
    el.disp = NAN;
    const char* type = e["type"] | "";

    // Common visibility gates (any element type). visible_if.chan must be a
    // valid scalar channel (bit channels aren't key-addressable).
    JsonObjectConst vi = e["visible_if"];
    if (!vi.isNull()) {
      long vk = vi["chan"] | 0L;
      el.vis_chan_idx = (int16_t)chan_index_from_key((uint16_t)vk);
      if (el.vis_chan_idx < 0) { err = String("visible_if unknown chan ") + vk; return false; }
      const char* op = vi["op"] | "truthy";
      el.vis_op = strcmp(op, ">") == 0 ? OP_GT : strcmp(op, "<") == 0 ? OP_LT
                : strcmp(op, ">=") == 0 ? OP_GE : strcmp(op, "<=") == 0 ? OP_LE
                : strcmp(op, "==") == 0 ? OP_EQ : strcmp(op, "!=") == 0 ? OP_NE : OP_TRUTHY;
      el.vis_value = vi["value"] | 0.0f;
    }
    el.stale_ms = (uint16_t)constrain((long)(e["stale_ms"] | 0L), 0L, 60000L);
    el.pulse = e["pulse"] | false;   // any element: opacity blink
    JsonObjectConst shd = e["shadow"];   // any element: blur shadow (heavy)
    el.has_shadow = !shd.isNull();
    if (el.has_shadow) {
      el.shadow_w = (int16_t)constrain((long)(shd["w"] | 8L), 0L, 100L);
      el.shadow_spread = (int16_t)constrain((long)(shd["spread"] | 0L), -30L, 30L);
      el.shadow_ox = (int16_t)constrain((long)(shd["x"] | 0L), -60L, 60L);
      el.shadow_oy = (int16_t)constrain((long)(shd["y"] | 0L), -60L, 60L);
      el.shadow_opa = (uint8_t)constrain((long)(shd["opa"] | 180L), 0L, 255L);
      el.shadow_col.role = -1; el.shadow_col.hex = 0x000000;
      if (!shd["color"].isNull()) parse_color(shd["color"], &el.shadow_col);
    }

    // shared helpers
    auto getChan = [&](bool required) -> bool {
      long key = e["chan"] | 0L;
      el.chan_idx = (int16_t)chan_index_from_key((uint16_t)key);
      if (required && el.chan_idx < 0) { err = String("unknown chan ") + key; return false; }
      return true;
    };
    auto getFont = [&]() -> bool {
      el.font = font_by_name(e["font"] | "");
      if (!el.font) { err = String("unknown font ") + (e["font"] | "?"); return false; }
      return true;
    };
    auto getAlign = [&]() {
      const char* a = e["align"] | "center";
      el.align = strcmp(a, "left") == 0 ? 0 : strcmp(a, "right") == 0 ? 2 : 1;
    };
    auto getXY = [&]() -> bool {
      long x = e["x"] | 0L, y = e["y"] | 0L;
      if (!coord_ok(x) || !coord_ok(y)) { err = "coord out of range"; return false; }
      el.x = (int16_t)x; el.y = (int16_t)y;
      return true;
    };
    auto getRange = [&]() -> bool {
      el.min = e["min"] | 0.0f; el.max = e["max"] | 100.0f;
      if (!(el.min < el.max)) { err = "min must be < max"; return false; }
      el.z1 = e["z1"] | -99999.0f; el.z2 = e["z2"] | 99999.0f;
      return true;
    };
    auto getCol = [&](const char* field, LeColor* c, bool required) -> bool {
      if (e[field].isNull()) { if (required) err = String("missing colour ") + field; return !required; }
      if (!parse_color(e[field], c)) { err = String("bad colour ") + field; return false; }
      return true;
    };

    if (strcmp(type, "text") == 0) {
      el.type = LE_TEXT;
      if (!getXY() || !getFont() || !getCol("color", &el.col, true)) return false;
      getAlign();
      strlcpy(el.str, e["str"] | "", sizeof(el.str));
    } else if (strcmp(type, "numeric") == 0) {
      el.type = LE_NUMERIC;
      if (!getChan(true) || !getXY() || !getFont() || !getCol("color", &el.col, true)) return false;
      getAlign();
      el.decimals = (uint8_t)constrain((int)(e["decimals"] | 1), 0, 3);
      el.unit = e["unit"] | false;
      strlcpy(el.str, e["prefix"] | "", sizeof(el.str));
    } else if (strcmp(type, "needle") == 0) {
      el.type = LE_NEEDLE;
      if (!getChan(true) || !getRange() || !getCol("color", &el.col, true)) return false;
      el.cx = (int16_t)(e["cx"] | 240L); el.cy = (int16_t)(e["cy"] | 240L);
      el.r0 = (int16_t)(e["r0"] | 185L); el.r1 = (int16_t)(e["r1"] | 225L);
      if (el.r0 < 0 || el.r1 <= el.r0) { err = "need 0 <= r0 < r1"; return false; }
      el.start_deg = (int16_t)(e["start_deg"] | 135L);
      el.sweep_deg = (int16_t)(e["sweep_deg"] | 270L);
      el.width = (uint8_t)constrain((int)(e["width"] | 8), 1, 40);
      el.peak_src = strcmp(e["source"] | "live", "peak") == 0;  // park at session max
      el.peak = -1e9f;
      el.img_needle = strcmp(e["style"] | "line", "image") == 0;  // rotated sprite (heavy)
    } else if (strcmp(type, "ring") == 0) {
      el.type = LE_RING;
      if (!getChan(true) || !getRange()) return false;
      if (!getCol("low", &el.low, true) || !getCol("mid", &el.mid, true) || !getCol("high", &el.high, true)) return false;
      el.cx = (int16_t)(e["cx"] | 240L); el.cy = (int16_t)(e["cy"] | 240L);
      el.r = (int16_t)(e["r"] | 240L); el.width = (uint8_t)constrain((int)(e["width"] | 16), 1, 60);
      if (el.r <= 0) { err = "ring r must be > 0"; return false; }
    } else if (strcmp(type, "bar") == 0) {
      el.type = LE_BAR;
      if (!getChan(true) || !getRange() || !getXY()) return false;
      if (!getCol("track", &el.track, true) || !getCol("low", &el.low, true)
          || !getCol("mid", &el.mid, true) || !getCol("high", &el.high, true)) return false;
      el.w = (int16_t)(e["w"] | 100L); el.h = (int16_t)(e["h"] | 16L);
      if (el.w <= 0 || el.h <= 0) { err = "bar w/h must be > 0"; return false; }
      el.horiz = strcmp(e["dir"] | "h", "v") != 0;
      el.radius = (uint8_t)constrain((int)(e["radius"] | 0), 0, 60);
      el.has_origin = !e["origin"].isNull();   // center-origin fill (boost/vacuum, ±)
      el.origin = e["origin"] | 0.0f;
    } else if (strcmp(type, "shiftlights") == 0) {
      el.type = LE_SHIFT;
      if (!getChan(true) || !getRange() || !getXY()) return false;
      if (!getCol("low", &el.low, true) || !getCol("mid", &el.mid, true) || !getCol("high", &el.high, true)) return false;
      el.segs = (uint8_t)constrain((int)(e["segs"] | 8), 1, 20);
      el.w = (int16_t)constrain((long)(e["seg_w"] | 20L), 1L, 200L);
      el.h = (int16_t)constrain((long)(e["seg_h"] | 40L), 1L, 200L);
      el.gap = (int16_t)constrain((long)(e["gap"] | 6L), 0L, 60L);
      el.radius = (uint8_t)constrain((int)(e["radius"] | 3), 0, 60);
      el.flash_max = e["flash_max"] | false;
    } else if (strcmp(type, "shape") == 0) {
      el.type = LE_SHAPE;
      const char* sh = e["shape"] | "rect";
      if (strcmp(sh, "line") == 0) {
        JsonArrayConst pts = e["points"];
        if (pts.isNull() || pts.size() < 2 || pts.size() > LE_MAX_LINE_PTS) {
          err = "line needs 2..8 points"; return false;
        }
        el.n_pts = (uint8_t)pts.size();
        int k = 0;
        for (JsonArrayConst p : pts) {
          long px = p[0] | 0L, py = p[1] | 0L;
          if (!coord_ok(px) || !coord_ok(py)) { err = "line point out of range"; return false; }
          if (apply) { s_pts[n][k][0] = (int16_t)px; s_pts[n][k][1] = (int16_t)py; }
          k++;
        }
        el.has_stroke = true;
        if (e["stroke"].isNull()) { el.stroke.role = -1; el.stroke.hex = 0xFFFFFF; }
        else if (!getCol("stroke", &el.stroke, true)) return false;
        el.stroke_w = (uint8_t)constrain((int)(e["stroke_w"] | 1), 1, 20);
        n++;
        continue;   // line fully parsed — skip rect/circle handling
      }
      if (!getXY()) return false;
      el.horiz = strcmp(sh, "rect") == 0;   // reuse: horiz => rect, else circle
      el.w = (int16_t)(e["w"] | 10L); el.h = (int16_t)(e["h"] | 10L);
      if (el.w <= 0 || el.h <= 0) { err = "shape w/h must be > 0"; return false; }
      el.radius = (uint8_t)constrain((int)(e["radius"] | 0), 0, 120);
      el.has_fill = !e["fill"].isNull();
      if (el.has_fill && !getCol("fill", &el.col, true)) return false;
      el.has_stroke = !e["stroke"].isNull();
      if (el.has_stroke && !getCol("stroke", &el.stroke, true)) return false;
      el.stroke_w = (uint8_t)constrain((int)(e["stroke_w"] | 1), 1, 20);
      if (!el.has_fill && !el.has_stroke) { err = "shape needs fill or stroke"; return false; }
    } else if (strcmp(type, "warning") == 0) {
      el.type = LE_WARNING;
      if (!getChan(true) || !getXY() || !getFont() || !getCol("on_color", &el.col, true)) return false;
      getAlign();
      strlcpy(el.str, e["on_str"] | "!", sizeof(el.str));
      el.off_hidden = e["off_hidden"] | true;
    } else if (strcmp(type, "scale") == 0) {
      el.type = LE_SCALE;   // static dial: ticks + labels + optional redline section
      if (!getRange() || !getCol("color", &el.col, true)) return false;
      el.cx = (int16_t)(e["cx"] | 240L); el.cy = (int16_t)(e["cy"] | 240L);
      el.r = (int16_t)(e["r"] | 220L);
      if (el.r <= 0) { err = "scale r must be > 0"; return false; }
      el.start_deg = (int16_t)(e["start_deg"] | 135L);
      el.sweep_deg = (int16_t)(e["sweep_deg"] | 270L);
      el.sc_major = (uint8_t)constrain((int)(e["major"] | 6), 2, 30);
      el.sc_minor = (uint8_t)constrain((int)(e["minor"] | 4), 0, 10);
      el.sc_maj_len = (int16_t)constrain((long)(e["maj_len"] | 14L), 1L, 60L);
      el.sc_min_len = (int16_t)constrain((long)(e["min_len"] | 7L), 1L, 60L);
      el.width = (uint8_t)constrain((int)(e["tick_w"] | 3), 1, 20);
      el.sc_labels = e["labels"] | false;
      if (el.sc_labels && !getFont()) return false;
      el.sc_from = e["sec_from"] | 1.0f; el.sc_to = e["sec_to"] | 0.0f;   // to<=from => no section
      if (el.sc_to > el.sc_from && !getCol("sec_color", &el.high, true)) return false;
    } else if (strcmp(type, "led") == 0) {
      el.type = LE_LED;
      if (!getCol("color", &el.col, true)) return false;
      el.cx = (int16_t)(e["cx"] | 240L); el.cy = (int16_t)(e["cy"] | 240L);
      el.r = (int16_t)constrain((long)(e["r"] | 12L), 2L, 120L);
      if (!getChan(false)) return false;                 // optional bind (unbound = always on)
      el.led_on = e["on_above"] | 0.5f;                  // on when display >= threshold
    } else if (strcmp(type, "image") == 0) {
      el.type = LE_IMAGE;
      if (!getXY()) return false;
      const char* icn = e["icon"] | "";
      el.icon = icon_by_name(icn);
      if (!el.icon) { err = String("unknown icon ") + icn; return false; }
      el.has_fill = !e["recolor"].isNull();              // reuse has_fill => recolour present
      if (el.has_fill && !getCol("recolor", &el.col, true)) return false;
    } else if (strcmp(type, "alert") == 0) {
      el.type = LE_ALERT;   // danger overlay; gate with visible_if, blink with pulse
      if (!getCol("color", &el.col, true)) return false;
      el.alert_full = strcmp(e["mode"] | "ring", "fullscreen") == 0;
      el.width = (uint8_t)constrain((int)(e["width"] | 20), 2, 120);   // ring thickness
      el.alert_opa = (uint8_t)constrain((int)(e["opa"] | 128), 0, 255); // fullscreen opacity
    } else {
      err = String("unknown element type ") + type;
      return false;
    }
    n++;
  }

  if (apply) {
    s_count = n;
    strlcpy(s_name, meta_name ? meta_name : "", sizeof(s_name));
    // Keep the background spec for rebuilds.
    s_bg_json[0] = 0;
    if (!page["bg"].isNull()) serializeJson(page["bg"], s_bg_json, sizeof(s_bg_json));
  }
  return true;
}

bool layout_validate(const String& json, String& err) {
  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) { err = "bad JSON"; return false; }
  int pc, si;
  if (!validate_top(doc, err, &pc, &si)) return false;
  // Validate EVERY page (not just the start page) so a bad far page is caught
  // on upload rather than crashing later when the user switches to it.
  int i = 0;
  for (JsonObjectConst p : doc["pages"].as<JsonArrayConst>()) {
    if (!parse_page_elements(p, nullptr, false, err)) { err = String("page ") + i + ": " + err; return false; }
    i++;
  }
  return true;
}

// ---------------- build ----------------
static void apply_layout_background() {
  lv_obj_set_style_bg_grad(gauge_scr, NULL, 0);
  uint32_t bgc = 0x000000;
  if (s_bg_json[0]) {
    JsonDocument bg;
    if (deserializeJson(bg, s_bg_json) == DeserializationError::Ok) {
      const char* t = bg["type"] | "solid";
      if (strcmp(t, "theme") == 0) bgc = color_background;
      else if (strcmp(t, "solid") == 0) {
        const char* c = bg["color"] | "#000000";
        if (c[0] == '#') bgc = strtoul(c + 1, nullptr, 16);
      } else if (strcmp(t, "gradient") == 0) {
        JsonObjectConst g = bg["grad"];
        auto hex = [](const char* s) -> uint32_t { return (s && s[0] == '#') ? strtoul(s + 1, nullptr, 16) : 0; };
        uint32_t c1 = hex(g["c1"] | "#000000"), c2 = hex(g["c2"] | "#000000"), c3 = hex(g["c3"] | "#000000");
        uint8_t stops = ((int)(g["stops"] | 2) == 3) ? 3 : 2;
        int angle = (int)(g["angle"] | 0) % 360;
        const char* kind = g["kind"] | (const char*)(g["type"] | "linear-v");
        lv_color_t cols[3] = { lv_color_hex(c1), lv_color_hex(c2), lv_color_hex(c3) };
        lv_grad_init_stops(&s_bg_grad, cols, NULL, NULL, stops);
        if (strcmp(kind, "linear-h") == 0)
          lv_grad_linear_init(&s_bg_grad, 0, 0, 480, 0, LV_GRAD_EXTEND_PAD);
        else if (strcmp(kind, "linear-angle") == 0) {
          float a = angle * 3.14159265f / 180.0f;
          int32_t dx = (int32_t)(cosf(a) * 340.0f), dy = (int32_t)(sinf(a) * 340.0f);
          lv_grad_linear_init(&s_bg_grad, 240 - dx, 240 - dy, 240 + dx, 240 + dy, LV_GRAD_EXTEND_PAD);
        } else if (strcmp(kind, "radial") == 0)
          lv_grad_radial_init(&s_bg_grad, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT, LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD);
        else if (strcmp(kind, "conical") == 0)
          lv_grad_conical_init(&s_bg_grad, LV_GRAD_CENTER, LV_GRAD_CENTER, angle, angle + 359, LV_GRAD_EXTEND_PAD);
        else
          lv_grad_linear_init(&s_bg_grad, 0, 0, 0, 480, LV_GRAD_EXTEND_PAD);
        lv_obj_set_style_bg_color(gauge_scr, cols[0], 0);
        lv_obj_set_style_bg_grad(gauge_scr, &s_bg_grad, 0);
        return;
      }
    }
  }
  lv_obj_set_style_bg_color(gauge_scr, lv_color_hex(bgc), 0);
}

static lv_text_align_t lv_align_of(uint8_t a) {
  return a == 0 ? LV_TEXT_ALIGN_LEFT : a == 2 ? LV_TEXT_ALIGN_RIGHT : LV_TEXT_ALIGN_CENTER;
}
// Place a label so its anchor (left/centre/right at x, vertical centre at y)
// matches the Designer/simulator convention (spec §4). Measures the text
// directly (no forced relayout — same discipline as the built-in face).
static void place_label(LeElement& el, const char* text) {
  lv_point_t sz;
  lv_text_get_size(&sz, text, el.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  int32_t x = el.align == 0 ? el.x : el.align == 2 ? el.x - sz.x : el.x - sz.x / 2;
  lv_obj_set_pos(el.obj, x, el.y - sz.y / 2);
}
// Numeric-with-unit: the value uses the element font (often a digit-only subset
// like DSEG/Fira Mono, so unit letters would tofu); the unit is a second label
// (el.obj2) in a full-glyph font. Both are vertically centred on el.y and the
// value+gap+unit group is anchored by el.align.
static void place_numeric(LeElement& el, const char* valtext) {
  lv_point_t vs;
  lv_text_get_size(&vs, valtext, el.font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  if (!el.obj2) {                        // no unit — identical to place_label
    int32_t x = el.align == 0 ? el.x : el.align == 2 ? el.x - vs.x : el.x - vs.x / 2;
    lv_obj_set_pos(el.obj, x, el.y - vs.y / 2);
    return;
  }
  const lv_font_t* uf = &lv_font_montserrat_20;
  lv_point_t us;
  lv_text_get_size(&us, lv_label_get_text(el.obj2), uf, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
  const int gap = 6;
  int32_t total = vs.x + gap + us.x;
  int32_t startx = el.align == 0 ? el.x : el.align == 2 ? el.x - total : el.x - total / 2;
  lv_obj_set_pos(el.obj, startx, el.y - vs.y / 2);
  lv_obj_set_pos(el.obj2, startx + vs.x + gap, el.y - us.y / 2);
}

static void build_element(LeElement& el, int idx) {
  switch (el.type) {
    case LE_TEXT: {
      el.obj = lv_label_create(gauge_scr);
      lv_obj_set_style_text_font(el.obj, el.font, 0);
      lv_obj_set_style_text_color(el.obj, lv_color_hex(col_val(el.col)), 0);
      lv_label_set_text(el.obj, el.str);
      place_label(el, el.str);
      break;
    }
    case LE_NUMERIC:
    case LE_WARNING: {
      el.obj = lv_label_create(gauge_scr);
      lv_obj_set_style_text_font(el.obj, el.font, 0);
      lv_obj_set_style_text_color(el.obj, lv_color_hex(col_val(el.col)), 0);
      lv_obj_set_style_text_align(el.obj, lv_align_of(el.align), 0);
      const char* initial = el.type == LE_WARNING ? el.str : "--";
      lv_label_set_text(el.obj, initial);
      if (el.type == LE_NUMERIC && el.unit) {   // separate full-glyph unit label
        el.obj2 = lv_label_create(gauge_scr);
        lv_obj_set_style_text_font(el.obj2, &lv_font_montserrat_20, 0);
        lv_obj_set_style_text_color(el.obj2, lv_color_hex(col_val(el.col)), 0);
        lv_label_set_text(el.obj2, chan_unit_str(el.chan_idx));
        place_numeric(el, initial);
      } else {
        place_label(el, initial);
      }
      if (el.type == LE_WARNING && el.off_hidden) lv_obj_add_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
      el.prev_a = -1;   // warning: last on/off state
      break;
    }
    case LE_NEEDLE: {
      if (el.img_needle) {   // rotated bitmap sprite (heavy — measured)
        el.obj = lv_image_create(gauge_scr);
        lv_image_set_src(el.obj, &icon_needle);
        lv_obj_set_style_image_recolor(el.obj, lv_color_hex(col_val(el.col)), 0);
        lv_obj_set_style_image_recolor_opa(el.obj, LV_OPA_COVER, 0);
        lv_image_set_pivot(el.obj, 8, 172);          // hub centre (NW/2, NH-8)
        lv_obj_set_pos(el.obj, el.cx - 8, el.cy - 172);
        el.prev_a = INT32_MIN;
      } else {
        el.obj = lv_line_create(gauge_scr);
        lv_obj_set_style_line_width(el.obj, el.width, 0);
        lv_obj_set_style_line_color(el.obj, lv_color_hex(col_val(el.col)), 0);
        lv_obj_set_style_line_rounded(el.obj, 0, 0);
        el.prev_a = el.prev_b = INT32_MIN;
      }
      break;
    }
    case LE_RING: {
      el.obj = lv_obj_create(gauge_scr);
      int d = el.r * 2;
      lv_obj_set_size(el.obj, d, d);
      lv_obj_set_pos(el.obj, el.cx - el.r, el.cy - el.r);
      lv_obj_set_style_bg_opa(el.obj, LV_OPA_TRANSP, 0);
      lv_obj_set_style_pad_all(el.obj, 0, 0);
      lv_obj_set_style_border_width(el.obj, el.width, 0);
      lv_obj_set_style_border_color(el.obj, lv_color_hex(col_val(el.low)), 0);
      lv_obj_set_style_border_side(el.obj, LV_BORDER_SIDE_FULL, 0);
      lv_obj_set_style_radius(el.obj, el.r, 0);
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      el.prev_col = 0;
      break;
    }
    case LE_BAR: {
      el.obj = lv_obj_create(gauge_scr);       // track
      lv_obj_remove_style_all(el.obj);
      lv_obj_set_pos(el.obj, el.x, el.y);
      lv_obj_set_size(el.obj, el.w, el.h);
      lv_obj_set_style_radius(el.obj, el.radius, 0);
      lv_obj_set_style_bg_opa(el.obj, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(el.obj, lv_color_hex(col_val(el.track)), 0);
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      el.obj2 = lv_obj_create(gauge_scr);      // fill
      lv_obj_remove_style_all(el.obj2);
      lv_obj_set_style_radius(el.obj2, el.radius, 0);
      lv_obj_set_style_bg_opa(el.obj2, LV_OPA_COVER, 0);
      lv_obj_set_style_bg_color(el.obj2, lv_color_hex(col_val(el.low)), 0);
      lv_obj_clear_flag(el.obj2, LV_OBJ_FLAG_SCROLLABLE);
      el.prev_a = -1; el.prev_col = 0;
      break;
    }
    case LE_SHAPE: {
      if (el.n_pts > 0) {   // polyline
        el.obj = lv_line_create(gauge_scr);
        lv_obj_set_style_line_width(el.obj, el.stroke_w, 0);
        lv_obj_set_style_line_color(el.obj, lv_color_hex(col_val(el.stroke)), 0);
        lv_obj_set_style_line_rounded(el.obj, 0, 0);
        for (int k = 0; k < el.n_pts; k++) {
          s_line_lv[idx][k].x = s_pts[idx][k][0];
          s_line_lv[idx][k].y = s_pts[idx][k][1];
        }
        lv_line_set_points(el.obj, s_line_lv[idx], el.n_pts);
        break;
      }
      el.obj = lv_obj_create(gauge_scr);
      lv_obj_remove_style_all(el.obj);
      lv_obj_set_pos(el.obj, el.x, el.y);
      lv_obj_set_size(el.obj, el.w, el.h);
      // horiz flag reused: true = rect (given radius), false = circle
      lv_obj_set_style_radius(el.obj, el.horiz ? el.radius : LV_RADIUS_CIRCLE, 0);
      if (el.has_fill) {
        lv_obj_set_style_bg_opa(el.obj, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(el.obj, lv_color_hex(col_val(el.col)), 0);
      } else {
        lv_obj_set_style_bg_opa(el.obj, LV_OPA_TRANSP, 0);
      }
      if (el.has_stroke) {
        lv_obj_set_style_border_width(el.obj, el.stroke_w, 0);
        lv_obj_set_style_border_color(el.obj, lv_color_hex(col_val(el.stroke)), 0);
      }
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      break;
    }
    case LE_SHIFT: {
      // Transparent container holding `segs` rects; each rect is pre-coloured by
      // its threshold zone (green→amber→red by position) and starts off. Update
      // only toggles bg_opa, so only the segments that changed invalidate.
      el.obj = lv_obj_create(gauge_scr);
      lv_obj_remove_style_all(el.obj);
      lv_obj_set_pos(el.obj, el.x, el.y);
      int totalw = el.segs * el.w + (el.segs - 1) * el.gap;
      lv_obj_set_size(el.obj, totalw, el.h);
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      for (int s = 0; s < el.segs; s++) {
        float thr = el.min + (s + 1) * (el.max - el.min) / el.segs;
        lv_obj_t* seg = lv_obj_create(el.obj);
        lv_obj_remove_style_all(seg);
        lv_obj_set_pos(seg, s * (el.w + el.gap), 0);
        lv_obj_set_size(seg, el.w, el.h);
        lv_obj_set_style_radius(seg, el.radius, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(zone_col(el, thr)), 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_TRANSP, 0);
        lv_obj_clear_flag(seg, LV_OBJ_FLAG_SCROLLABLE);
      }
      el.prev_a = -1; el.prev_b = -1;
      break;
    }
    case LE_SCALE: {
      el.obj = lv_scale_create(gauge_scr);
      lv_scale_set_mode(el.obj, LV_SCALE_MODE_ROUND_INNER);
      int d = el.r * 2;
      lv_obj_set_size(el.obj, d, d);
      lv_obj_set_pos(el.obj, el.cx - el.r, el.cy - el.r);
      lv_obj_set_style_bg_opa(el.obj, LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(el.obj, 0, 0);
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      lv_scale_set_rotation(el.obj, el.start_deg);
      lv_scale_set_angle_range(el.obj, el.sweep_deg);
      lv_scale_set_range(el.obj, (int32_t)el.min, (int32_t)el.max);
      uint32_t total = (uint32_t)(el.sc_major - 1) * (el.sc_minor + 1) + 1;
      lv_scale_set_total_tick_count(el.obj, total);
      lv_scale_set_major_tick_every(el.obj, el.sc_minor + 1);
      lv_scale_set_label_show(el.obj, el.sc_labels);
      uint32_t tc = col_val(el.col);
      lv_obj_set_style_line_color(el.obj, lv_color_hex(tc), LV_PART_ITEMS);       // minor ticks
      lv_obj_set_style_length(el.obj, el.sc_min_len, LV_PART_ITEMS);
      lv_obj_set_style_line_width(el.obj, el.width, LV_PART_ITEMS);
      lv_obj_set_style_line_color(el.obj, lv_color_hex(tc), LV_PART_INDICATOR);   // major ticks + labels
      lv_obj_set_style_length(el.obj, el.sc_maj_len, LV_PART_INDICATOR);
      lv_obj_set_style_line_width(el.obj, el.width + 1, LV_PART_INDICATOR);
      lv_obj_set_style_text_color(el.obj, lv_color_hex(tc), LV_PART_INDICATOR);
      if (el.sc_labels) lv_obj_set_style_text_font(el.obj, el.font, LV_PART_INDICATOR);
      if (el.sc_to > el.sc_from) {                 // redline section
        lv_style_t* st = &s_sec_style[idx];
        if (s_sec_used[idx]) lv_style_reset(st);
        lv_style_init(st);
        s_sec_used[idx] = true;
        uint32_t sc = col_val(el.high);
        lv_style_set_line_color(st, lv_color_hex(sc));
        lv_style_set_line_width(st, el.width + 1);
        lv_style_set_length(st, el.sc_maj_len);
        lv_style_set_text_color(st, lv_color_hex(sc));
        lv_scale_section_t* sec = lv_scale_add_section(el.obj);
        lv_scale_set_section_range(el.obj, sec, (int32_t)el.sc_from, (int32_t)el.sc_to);
        lv_scale_set_section_style_indicator(el.obj, sec, st);
        lv_scale_set_section_style_items(el.obj, sec, st);
      }
      break;
    }
    case LE_LED: {
      el.obj = lv_led_create(gauge_scr);
      lv_obj_set_pos(el.obj, el.cx - el.r, el.cy - el.r);
      lv_obj_set_size(el.obj, el.r * 2, el.r * 2);
      lv_led_set_color(el.obj, lv_color_hex(col_val(el.col)));
      if (el.chan_idx < 0) lv_led_on(el.obj);    // unbound = static indicator
      else lv_led_off(el.obj);
      el.prev_a = -1;
      break;
    }
    case LE_IMAGE: {
      el.obj = lv_image_create(gauge_scr);
      lv_image_set_src(el.obj, (const lv_image_dsc_t*)el.icon);
      // A8 icons carry only coverage; recolour supplies the pixel colour.
      lv_obj_set_style_image_recolor(el.obj, lv_color_hex(el.has_fill ? col_val(el.col) : 0xFFFFFF), 0);
      lv_obj_set_style_image_recolor_opa(el.obj, LV_OPA_COVER, 0);
      lv_obj_set_pos(el.obj, el.x, el.y);
      break;
    }
    case LE_ALERT: {
      el.obj = lv_obj_create(gauge_scr);
      lv_obj_remove_style_all(el.obj);
      lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_SCROLLABLE);
      if (el.alert_full) {                 // whole-screen flash (heavy — measured)
        lv_obj_set_pos(el.obj, 0, 0);
        lv_obj_set_size(el.obj, 480, 480);
        lv_obj_set_style_radius(el.obj, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(el.obj, lv_color_hex(col_val(el.col)), 0);
        lv_obj_set_style_bg_opa(el.obj, el.alert_opa, 0);
      } else {                             // edge ring (small invalidated area)
        int r = 238;
        lv_obj_set_pos(el.obj, 240 - r, 240 - r);
        lv_obj_set_size(el.obj, r * 2, r * 2);
        lv_obj_set_style_radius(el.obj, r, 0);
        lv_obj_set_style_bg_opa(el.obj, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(el.obj, el.width, 0);
        lv_obj_set_style_border_color(el.obj, lv_color_hex(col_val(el.col)), 0);
      }
      break;
    }
  }
  // Any element can opt into a pulsing (blinking) opacity — EXCEPT alert: it is
  // screen-sized, and LVGL invalidates by bounding box, so pulsing it repaints
  // the whole screen every cycle (measured R90). A static alert that appears on
  // its visible_if trigger costs one redraw per transition — cheap in both modes.
  if (el.pulse && el.obj && el.type != LE_ALERT) start_pulse(el.obj);
  // ...or a blur shadow (heavy — quantified in the tier-3 round).
  if (el.has_shadow && el.obj) {
    lv_obj_set_style_shadow_width(el.obj, el.shadow_w, 0);
    lv_obj_set_style_shadow_spread(el.obj, el.shadow_spread, 0);
    lv_obj_set_style_shadow_offset_x(el.obj, el.shadow_ox, 0);
    lv_obj_set_style_shadow_offset_y(el.obj, el.shadow_oy, 0);
    lv_obj_set_style_shadow_color(el.obj, lv_color_hex(col_val(el.shadow_col)), 0);
    lv_obj_set_style_shadow_opa(el.obj, el.shadow_opa, 0);
  }
}

// Tear down whatever is on gauge_scr and build the currently-parsed page
// (s_el/s_count/s_bg_json). Runs on loopTask.
static void build_active_scene() {
  // Slow the LCD DMA before the render burst so the bounce buffer can't underrun
  // (an underrun slips the DMA phase = vertical shift). The pclk change applies on
  // the next VSYNC, so settle briefly to guarantee it's live before ANY rendering
  // starts (otherwise the first burst frame can still underrun at full clock).
  // Full speed is restored in loop() once the burst has settled (lcd_resync_at_ms).
  lcd_set_pclk(LCD_PCLK_RELOAD_HZ);
  vTaskDelay(pdMS_TO_TICKS(30));
  lv_obj_clean(gauge_scr);
  // The built-in face's objects were just destroyed — null the shared handles
  // so loop()'s flag handlers (link icon, stats overlay, mode label...) and
  // apply_theme_colors() can't touch dangling pointers while a layout is up.
  val_label_int = val_label_dec = mode_label = link_icon = nullptr;
  bar = peak_dot = peak_high_label = peak_low_label = perf_label = needle_tip = nullptr;

  apply_layout_background();
  for (int i = 0; i < s_count; i++) build_element(s_el[i], i);

  // Recreate the perf overlay so on-glass FPS works while a layout is active
  // (lv_obj_clean above destroyed the built-in face's perf_label). Kept on top,
  // out of the dial centre; loop()'s STATS block drives the text.
  perf_label = lv_label_create(gauge_scr);
  lv_obj_align(perf_label, LV_ALIGN_TOP_MID, 0, 6);
  lv_obj_set_style_text_color(perf_label, lv_color_white(), 0);
  lv_obj_set_style_bg_color(perf_label, lv_color_black(), 0);
  lv_obj_set_style_bg_opa(perf_label, 150, 0);
  lv_label_set_text(perf_label, "FPS:--");
  if (!show_perf_stats) lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);

  // Post-rebuild recovery (see loop()): restore full pclk after the burst, and
  // continuously re-align the DMA for a window so any slip is corrected in a frame.
  lcd_resync_at_ms = millis() + 400;
  lcd_resync_until_ms = millis() + 1500;
}

// Read + parse the stored layout, build one page (page_idx, or the start page
// when page_idx < 0). Returns false (and leaves s_active false) on any failure.
static bool load_page(int page_idx) {
  String json = layout_store_read();
  if (json.length() == 0) return false;

  JsonDocument doc;
  if (deserializeJson(doc, json) != DeserializationError::Ok) {
    Serial.println("[LAYOUT] stored layout is not valid JSON — using default face");
    return false;
  }
  int pc, start_idx; String err;
  if (!validate_top(doc, err, &pc, &start_idx)) {
    Serial.printf("[LAYOUT] stored layout invalid (%s) — using default face\n", err.c_str());
    return false;
  }
  int idx = (page_idx < 0) ? start_idx : page_idx;
  if (idx < 0 || idx >= pc) return false;

  JsonArrayConst pages = doc["pages"];
  if (!parse_page_elements(pages[idx], doc["meta"]["name"] | "", true, err)) {
    Serial.printf("[LAYOUT] page %d invalid (%s) — using default face\n", idx, err.c_str());
    s_count = 0;
    return false;
  }
  s_page_count = pc;
  s_active_page = idx;
  build_active_scene();
  s_active = true;
  Serial.printf("[LAYOUT] active: \"%s\" page %d/%d (%d elements)\n", s_name, idx, pc, s_count);
  return true;
}

bool layout_engine_load() {
  layout_engine_unload();
  return load_page(-1);   // start page
}

// Switch to a different page of the current layout (rebuilds the scene).
bool layout_engine_set_page(int idx) {
  if (!s_active || idx < 0 || idx >= s_page_count) return false;
  return load_page(idx);
}

void layout_engine_unload() {
  s_active = false;
  s_count = 0;
  // Caller rebuilds the built-in face (load_current_style cleans gauge_scr).
}

bool layout_engine_active() { return s_active; }
const char* layout_engine_name() { return s_name; }
int layout_engine_page() { return s_active_page; }
int layout_engine_page_count() { return s_page_count; }

void layout_engine_theme_changed() {
  if (!s_active) return;
  load_page(s_active_page);   // rebuild CURRENT page: re-resolves tokens + bg
}

// ---------------- per-frame update ----------------
static float smoothed(LeElement& el, float target) {
  if (isnan(el.disp)) { el.disp = target; return el.disp; }
  float delta = target - el.disp;
  if (fabsf(delta) < 0.05f) { el.disp = target; return el.disp; }
  el.disp += delta * behavior.smoothing;
  return el.disp;
}

static uint32_t zone_col(const LeElement& el, float v) {
  if (v < el.z1) return col_val(el.low);
  if (v < el.z2) return col_val(el.mid);
  return col_val(el.high);
}

// visible_if + stale_ms gate. Returns true if the element should be shown.
static bool gate_shown(const LeElement& el) {
  if (el.vis_chan_idx >= 0) {
    bool ok;
    if (el.vis_op == OP_TRUTHY) ok = haltech_value(el.vis_chan_idx) != 0.0f;
    else {
      float v = chan_display(el.vis_chan_idx, haltech_value(el.vis_chan_idx));
      switch (el.vis_op) {
        case OP_GT: ok = v >  el.vis_value; break;
        case OP_LT: ok = v <  el.vis_value; break;
        case OP_GE: ok = v >= el.vis_value; break;
        case OP_LE: ok = v <= el.vis_value; break;
        case OP_EQ: ok = v == el.vis_value; break;
        case OP_NE: ok = v != el.vis_value; break;
        default:    ok = true;
      }
    }
    if (!ok) return false;
  }
  if (el.stale_ms > 0 && el.chan_idx >= 0 && haltech_age_ms(el.chan_idx) > el.stale_ms)
    return false;
  return true;
}

void layout_engine_update() {
  if (!s_active) return;
  for (int i = 0; i < s_count; i++) {
    LeElement& el = s_el[i];

    // Visibility gates apply to EVERY element type (incl. static text/shape).
    if (el.vis_chan_idx >= 0 || el.stale_ms > 0) {
      int8_t shown = gate_shown(el) ? 1 : 0;
      if (shown != el.prev_shown) {
        el.prev_shown = shown;
        if (shown) {
          lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
          if (el.obj2) lv_obj_clear_flag(el.obj2, LV_OBJ_FLAG_HIDDEN);
          // invalidate per-type caches so the element repaints on re-show
          el.prev_a = el.prev_b = INT32_MIN; el.prev_col = 0; el.prev_txt[0] = '\x01'; el.prev_txt[1] = '\0';
        } else {
          lv_obj_add_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
          if (el.obj2) lv_obj_add_flag(el.obj2, LV_OBJ_FLAG_HIDDEN);
        }
      }
      if (!shown) continue;   // hidden — skip the per-type update
    }

    if (el.chan_idx < 0) continue;
    float display = chan_display(el.chan_idx, haltech_value(el.chan_idx));

    switch (el.type) {
      case LE_NUMERIC: {
        float v = smoothed(el, display);
        char buf[LE_STR_MAX + 20];
        snprintf(buf, sizeof(buf), "%s%.*f", el.str, el.decimals, v);   // unit is its own label
        if (strcmp(buf, el.prev_txt) != 0) {
          lv_label_set_text(el.obj, buf);
          strlcpy(el.prev_txt, buf, sizeof(el.prev_txt));
          place_numeric(el, buf);   // re-anchor value (+unit): width changes with the text
        }
        break;
      }
      case LE_NEEDLE: {
        float v;
        if (el.peak_src) { if (display > el.peak) el.peak = display; v = el.peak; }
        else v = smoothed(el, display);
        float norm = (v - el.min) / (el.max - el.min);
        norm = norm < 0 ? 0 : norm > 1 ? 1 : norm;
        float deg = el.start_deg + norm * el.sweep_deg;
        if (el.img_needle) {   // rotate the sprite (LVGL uses 0.1° clockwise units)
          // sprite points up (-y) at 0°; dial angle grows from +x toward +y → +90°.
          int32_t r10 = ((int32_t)((deg + 90.0f) * 10.0f)) % 3600;
          if (r10 < 0) r10 += 3600;
          if (r10 != el.prev_a) { lv_image_set_rotation(el.obj, r10); el.prev_a = r10; }
          break;
        }
        float rad = deg * (float)M_PI / 180.0f;
        static lv_point_precise_t pts[LE_MAX_ELEMENTS][2];   // persist per element
        int32_t x0 = el.cx + (int)(el.r0 * cosf(rad)), y0 = el.cy + (int)(el.r0 * sinf(rad));
        int32_t x1 = el.cx + (int)(el.r1 * cosf(rad)), y1 = el.cy + (int)(el.r1 * sinf(rad));
        int32_t ha = (x0 << 16) ^ y0, hb = (x1 << 16) ^ y1;
        if (ha != el.prev_a || hb != el.prev_b) {
          pts[i][0] = { (lv_value_precise_t)x0, (lv_value_precise_t)y0 };
          pts[i][1] = { (lv_value_precise_t)x1, (lv_value_precise_t)y1 };
          lv_line_set_points(el.obj, pts[i], 2);
          el.prev_a = ha; el.prev_b = hb;
        }
        break;
      }
      case LE_RING: {
        uint32_t c = zone_col(el, smoothed(el, display));
        if (c != el.prev_col) {
          lv_obj_set_style_border_color(el.obj, lv_color_hex(c), 0);
          el.prev_col = c;
        }
        break;
      }
      case LE_BAR: {
        float v = smoothed(el, display);
        int32_t span = el.horiz ? el.w : el.h;
        auto px = [&](float val) -> int32_t {
          float n = (val - el.min) / (el.max - el.min);
          n = n < 0 ? 0 : n > 1 ? 1 : n;
          return (int32_t)(span * n);
        };
        int32_t vp = px(v);
        int32_t lo, hi;
        if (el.has_origin) { int32_t op = px(el.origin); lo = op < vp ? op : vp; hi = op < vp ? vp : op; }
        else { lo = 0; hi = vp; }
        int32_t raw = hi - lo;                 // 0 = empty
        int32_t len = raw < 1 ? 1 : raw;
        uint32_t c = zone_col(el, v);
        int32_t hash = (lo << 16) | (len & 0xFFFF);
        if (hash != el.prev_a || c != el.prev_col) {
          if (el.horiz) {
            lv_obj_set_pos(el.obj2, el.x + lo, el.y);
            lv_obj_set_size(el.obj2, len, el.h);
          } else {
            lv_obj_set_pos(el.obj2, el.x, el.y + el.h - hi);
            lv_obj_set_size(el.obj2, el.w, len);
          }
          lv_obj_set_style_bg_opa(el.obj2, raw > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
          lv_obj_set_style_bg_color(el.obj2, lv_color_hex(c), 0);
          el.prev_a = hash; el.prev_col = c;
        }
        break;
      }
      case LE_SHIFT: {
        float v = smoothed(el, display);
        int32_t lit = 0;
        for (int s = 0; s < el.segs; s++) {
          float thr = el.min + (s + 1) * (el.max - el.min) / el.segs;
          if (v >= thr) lit |= (1 << s);
        }
        // flash phase: -1 = not flashing, 0 = blink-off, 1 = blink-on (all lit)
        int32_t flash = (el.flash_max && v >= el.max) ? (int32_t)((millis() / 110) & 1) : -1;
        if (lit != el.prev_a || flash != el.prev_b) {
          uint32_t count = lv_obj_get_child_count(el.obj);
          for (uint32_t s = 0; s < count; s++) {
            bool on = (lit >> s) & 1;
            if (flash == 0) on = false;
            else if (flash == 1) on = true;
            lv_obj_set_style_bg_opa(lv_obj_get_child(el.obj, s), on ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
          }
          el.prev_a = lit; el.prev_b = flash;
        }
        break;
      }
      case LE_LED: {
        int32_t on = display >= el.led_on ? 1 : 0;
        if (on != el.prev_a) { if (on) lv_led_on(el.obj); else lv_led_off(el.obj); el.prev_a = on; }
        break;
      }
      case LE_WARNING: {
        int32_t on = haltech_value(el.chan_idx) != 0.0f ? 1 : 0;
        if (on != el.prev_a) {
          if (on) lv_obj_clear_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
          else if (el.off_hidden) lv_obj_add_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
          el.prev_a = on;
        }
        break;
      }
      default: break;   // text/shape are static
    }
  }
}
