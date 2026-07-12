#include "layout_engine.h"
#include "render_shared.h"
#include "../layout_store.h"
#include "../haltech_channels.h"
#include "../haltech_decode.h"
#include "../themes.h"
#include <ArduinoJson.h>
#include <lvgl.h>
#include <math.h>

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

enum LeType : uint8_t { LE_TEXT, LE_NUMERIC, LE_NEEDLE, LE_RING, LE_BAR, LE_SHAPE, LE_WARNING };
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
      place_label(el, initial);
      if (el.type == LE_WARNING && el.off_hidden) lv_obj_add_flag(el.obj, LV_OBJ_FLAG_HIDDEN);
      el.prev_a = -1;   // warning: last on/off state
      break;
    }
    case LE_NEEDLE: {
      el.obj = lv_line_create(gauge_scr);
      lv_obj_set_style_line_width(el.obj, el.width, 0);
      lv_obj_set_style_line_color(el.obj, lv_color_hex(col_val(el.col)), 0);
      lv_obj_set_style_line_rounded(el.obj, 0, 0);
      el.prev_a = el.prev_b = INT32_MIN;
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
  }
}

// Tear down whatever is on gauge_scr and build the currently-parsed page
// (s_el/s_count/s_bg_json). Runs on loopTask.
static void build_active_scene() {
  lv_obj_clean(gauge_scr);
  // The built-in face's objects were just destroyed — null the shared handles
  // so loop()'s flag handlers (link icon, stats overlay, mode label...) and
  // apply_theme_colors() can't touch dangling pointers while a layout is up.
  val_label_int = val_label_dec = mode_label = link_icon = nullptr;
  bar = peak_dot = peak_high_label = peak_low_label = perf_label = needle_tip = nullptr;

  apply_layout_background();
  for (int i = 0; i < s_count; i++) build_element(s_el[i], i);
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
        snprintf(buf, sizeof(buf), "%s%.*f%s%s", el.str, el.decimals, v,
                 el.unit ? " " : "", el.unit ? chan_unit_str(el.chan_idx) : "");
        if (strcmp(buf, el.prev_txt) != 0) {
          lv_label_set_text(el.obj, buf);
          strlcpy(el.prev_txt, buf, sizeof(el.prev_txt));
          place_label(el, buf);   // re-anchor: width changes with the text
        }
        break;
      }
      case LE_NEEDLE: {
        float v = smoothed(el, display);
        float norm = (v - el.min) / (el.max - el.min);
        norm = norm < 0 ? 0 : norm > 1 ? 1 : norm;
        float rad = (el.start_deg + norm * el.sweep_deg) * (float)M_PI / 180.0f;
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
        float norm = (v - el.min) / (el.max - el.min);
        norm = norm < 0 ? 0 : norm > 1 ? 1 : norm;
        int32_t fill = (int32_t)((el.horiz ? el.w : el.h) * norm);
        uint32_t c = zone_col(el, v);
        if (fill != el.prev_a || c != el.prev_col) {
          if (el.horiz) {
            lv_obj_set_pos(el.obj2, el.x, el.y);
            lv_obj_set_size(el.obj2, fill > 0 ? fill : 1, el.h);
          } else {
            lv_obj_set_pos(el.obj2, el.x, el.y + el.h - fill);
            lv_obj_set_size(el.obj2, el.w, fill > 0 ? fill : 1);
          }
          lv_obj_set_style_bg_opa(el.obj2, fill > 0 ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
          lv_obj_set_style_bg_color(el.obj2, lv_color_hex(c), 0);
          el.prev_a = fill; el.prev_col = c;
        }
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
