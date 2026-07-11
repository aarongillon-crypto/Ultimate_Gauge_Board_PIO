#include "gauge_page.h"
#include "render_shared.h"
#include "../haltech_decode.h"
#include "../fleet.h"
#include <math.h>

LV_FONT_DECLARE(dseg14_96);
LV_FONT_DECLARE(dseg14_120);

#define FONT_FIRAMONO_AVAILABLE 1
#if FONT_FIRAMONO_AVAILABLE
LV_FONT_DECLARE(firamono_96);
LV_FONT_DECLARE(firamono_120);
#endif

// LVGL handles (declared in render_shared.h, owned by loopTask).
lv_obj_t *val_label_int;
lv_obj_t *val_label_dec;
lv_obj_t *mode_label;
lv_obj_t *link_icon;
lv_obj_t *bar;
lv_obj_t *peak_dot;
lv_obj_t *peak_high_label;
lv_obj_t *peak_low_label;
lv_obj_t *perf_label;
lv_obj_t *needle_tip;
lv_obj_t *gauge_scr = nullptr;
lv_obj_t *glowcraft_scr = nullptr;

static uint32_t current_applied_text = 0;

static void common_label_setup() {
  val_label_int = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_int, true, 0);

  val_label_dec = lv_label_create(gauge_scr);
  lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
  lv_obj_set_style_clip_corner(val_label_dec, true, 0);

    mode_label = lv_label_create(gauge_scr);
    #ifdef LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_28, 0);
    #else
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_14, 0);
    #endif
    lv_obj_set_style_text_color(mode_label, lv_color_hex(color_mode_label), 0);
    lv_label_set_text(mode_label, behavior.mode[current_mode].label);
#if FONT_FIRAMONO_AVAILABLE
    const lv_font_t* font_large = (current_font == 1) ? &firamono_120 : &dseg14_120;
    const lv_font_t* font_mid   = (current_font == 1) ? &firamono_96  : &dseg14_96;
#else
    const lv_font_t* font_large = &dseg14_120;
    const lv_font_t* font_mid   = &dseg14_96;
#endif
    lv_obj_set_style_text_font(val_label_int, font_large, 0);
    lv_obj_set_style_text_font(val_label_dec, font_mid, 0);
}

// Paint the gauge-screen background: a flat colour when bg_grad_type==0, otherwise a
// 2/3-stop gradient (vertical, horizontal, angled-linear, radial, or conical). The
// descriptor must persist — the style stores a pointer to it, not a copy — so it is
// static. Screen is the fixed 480x480 round panel; centre is (240,240).
static lv_grad_dsc_t bg_grad_dsc;
static void apply_background(lv_obj_t *scr) {
    lv_obj_set_style_bg_color(scr, lv_color_hex(color_background), 0);  // stop 1 / solid fallback
    if (bg_grad_type == 0) {
        lv_obj_set_style_bg_grad(scr, NULL, 0);  // clear any gradient left by a previous slot
        return;
    }
    uint8_t n = (bg_grad_stops == 3) ? 3 : 2;
    lv_color_t cols[3] = { lv_color_hex(color_background),
                           lv_color_hex(color_background2),
                           lv_color_hex(color_background3) };
    lv_grad_init_stops(&bg_grad_dsc, cols, NULL, NULL, n);  // NULL fracs/opa = even stops, opaque

    const int32_t W = 480, H = 480, CX = 240, CY = 240;
    switch (bg_grad_type) {
        case 1:  // linear vertical (top -> bottom)
            lv_grad_linear_init(&bg_grad_dsc, 0, 0, 0, H, LV_GRAD_EXTEND_PAD); break;
        case 2:  // linear horizontal (left -> right)
            lv_grad_linear_init(&bg_grad_dsc, 0, 0, W, 0, LV_GRAD_EXTEND_PAD); break;
        case 3: { // linear at an angle, through the centre
            float a = bg_grad_angle * 3.14159265f / 180.0f;
            int32_t dx = (int32_t)(cosf(a) * 340.0f), dy = (int32_t)(sinf(a) * 340.0f);
            lv_grad_linear_init(&bg_grad_dsc, CX - dx, CY - dy, CX + dx, CY + dy, LV_GRAD_EXTEND_PAD);
            break;
        }
        case 4:  // radial: centre -> right edge (radius 240)
            lv_grad_radial_init(&bg_grad_dsc, LV_GRAD_CENTER, LV_GRAD_CENTER, LV_GRAD_RIGHT, LV_GRAD_CENTER, LV_GRAD_EXTEND_PAD); break;
        case 5: {  // conical sweep starting at bg_grad_angle (keep angles in 0..360)
            int16_t a0 = (int16_t)(bg_grad_angle % 360);
            lv_grad_conical_init(&bg_grad_dsc, LV_GRAD_CENTER, LV_GRAD_CENTER, a0, a0 + 359, LV_GRAD_EXTEND_PAD);
            break;
        }
        default:
            lv_obj_set_style_bg_grad(scr, NULL, 0); return;
    }
    lv_obj_set_style_bg_grad(scr, &bg_grad_dsc, 0);
}

void load_current_style() {
    lv_obj_clean(gauge_scr);
    apply_background(gauge_scr);

    // LINK ICON
    link_icon = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(link_icon, &lv_font_montserrat_20, 0);
    lv_label_set_text(link_icon, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(link_icon, lv_color_hex(color_link_icon), 0);
    lv_obj_align(link_icon, LV_ALIGN_BOTTOM_MID, 0, -100);
    if(fleet_count == 0) lv_obj_add_flag(link_icon, LV_OBJ_FLAG_HIDDEN);

    // PERF OVERLAY (MOVED TO CENTER-TOP)
    perf_label = lv_label_create(gauge_scr);
    lv_obj_align(perf_label, LV_ALIGN_CENTER, 0, -140); // Move performance monitor above main text
    lv_obj_set_style_text_color(perf_label, lv_color_white(), 0);
    lv_obj_set_style_bg_color(perf_label, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(perf_label, 150, 0);
    if(!show_perf_stats) lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);

    // === STATIC RING INDICATOR (outer border, color-changing) ===
    bar = lv_obj_create(gauge_scr);
    const int ring_size = 480; // square (w == h)
    lv_obj_set_size(bar, ring_size, ring_size);  // Fill most of screen
    lv_obj_align(bar, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);  // Transparent background
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_set_style_border_width(bar, 16, 0);  // Thick border for ring
    lv_obj_set_style_border_color(bar, lv_color_hex(color_low), 0);  // Start with low color
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_radius(bar, ring_size / 2, 0);  // Full circle (half of size)

    peak_dot = lv_obj_create(gauge_scr);
    lv_obj_set_size(peak_dot, 8, 8);  // Small indicator dot (hidden for now)
    lv_obj_set_style_radius(peak_dot, 4, 0);
    lv_obj_set_style_bg_color(peak_dot, lv_color_hex(color_peak), 0);
    lv_obj_set_style_border_width(peak_dot, 0, 0);
    lv_obj_set_pos(peak_dot, 0, 0);
    if(!peak_hold_enabled) lv_obj_add_flag(peak_dot, LV_OBJ_FLAG_HIDDEN); // Initial State

    // NEEDLE TIP - thin triangle that orbits around center
    needle_tip = lv_line_create(gauge_scr);
    lv_obj_set_style_line_width(needle_tip, 8, 0);  // Thin line width
    lv_obj_set_style_line_color(needle_tip, lv_color_hex(needle_color), 0);
    lv_obj_set_style_line_rounded(needle_tip, 0, 0);  // Not rounded

    // PEAK HIGH / LOW LABELS (top section, same font as mode label)
    peak_high_label = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(peak_high_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(peak_high_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_align(peak_high_label, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_text(peak_high_label, LV_SYMBOL_UP " --");
    lv_obj_align(peak_high_label, LV_ALIGN_CENTER, -60, -120);
    if (!peak_hold_enabled) lv_obj_add_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);

    peak_low_label = lv_label_create(gauge_scr);
    lv_obj_set_style_text_font(peak_low_label, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(peak_low_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_align(peak_low_label, LV_TEXT_ALIGN_LEFT, 0);
    lv_label_set_text(peak_low_label, LV_SYMBOL_DOWN " --");
    lv_obj_align(peak_low_label, LV_ALIGN_CENTER, 60, -120);
    if (!peak_hold_enabled) lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);

    common_label_setup();
    lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -60);
    lv_obj_align(val_label_int, LV_ALIGN_CENTER, 50, -10); // Move right 40 px, up 10 px
    lv_obj_align(val_label_dec, LV_ALIGN_CENTER, 50, -10);  // Move right 40 px, up 10 px

    current_applied_text = 0;
}

// Apply the current theme's colours (plus background gradient and value-label
// font) to the EXISTING gauge objects, WITHOUT tearing the screen down. Every
// theme/colour/gradient/font change routes here. A full rebuild via
// load_current_style() on each switch fragmented LVGL's heap until glyph
// allocation failed, rendering text as boxes; updating styles in place avoids
// that churn entirely (and is far faster).
void apply_theme_colors() {
    if (!gauge_scr || !val_label_int) return;  // gauge UI not built yet
    apply_background(gauge_scr);
    lv_obj_set_style_text_color(val_label_int,   lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(val_label_dec,   lv_color_hex(text_color), 0);
    lv_obj_set_style_text_color(mode_label,      lv_color_hex(color_mode_label), 0);
    lv_obj_set_style_text_color(link_icon,       lv_color_hex(color_link_icon), 0);
    lv_obj_set_style_text_color(peak_high_label, lv_color_hex(color_peak), 0);
    lv_obj_set_style_text_color(peak_low_label,  lv_color_hex(color_peak), 0);
    lv_obj_set_style_bg_color(peak_dot,          lv_color_hex(color_peak), 0);
    lv_obj_set_style_line_color(needle_tip,      lv_color_hex(needle_color), 0);
    lv_obj_set_style_border_color(bar,           lv_color_hex(color_low), 0);  // update_ui refreshes live
#if FONT_FIRAMONO_AVAILABLE
    const lv_font_t* font_large = (current_font == 1) ? &firamono_120 : &dseg14_120;
    const lv_font_t* font_mid   = (current_font == 1) ? &firamono_96  : &dseg14_96;
    lv_obj_set_style_text_font(val_label_int, font_large, 0);
    lv_obj_set_style_text_font(val_label_dec, font_mid, 0);
#endif
}

void apply_page() {
  lv_scr_load(current_page == PAGE_GLOWCRAFT ? glowcraft_scr : gauge_scr);
}

// Secondary readout: any registry channel by chan_key. Short uppercase name +
// value + display unit; booleans/enums get sensible text (gear: N/R/number).
static void format_secondary(uint16_t chan, char* buf, size_t sz) {
    int idx = chan_index_from_key(chan);
    if (idx < 0) { snprintf(buf, sz, "--"); return; }
    const HaltechChannel& c = HALTECH_CHANNELS[idx];
    float v = haltech_value(idx);

    // Short label: first 10 chars of the channel name, uppercased.
    char nm[11];
    strncpy(nm, c.name, sizeof(nm) - 1); nm[sizeof(nm) - 1] = 0;
    for (char* p = nm; *p; p++) *p = toupper((unsigned char)*p);

    if (c.unit == U_BOOL) {
        snprintf(buf, sz, "%s %s", nm, v != 0 ? "ON" : "OFF");
    } else if (c.unit == U_ENUM && strcmp(c.name, "Gear") == 0) {
        int g = (int)v;
        if (g == 0)     snprintf(buf, sz, "GEAR N");
        else if (g < 0) snprintf(buf, sz, "GEAR R");
        else            snprintf(buf, sz, "GEAR %d", g);
    } else {
        snprintf(buf, sz, "%s %.1f%s", nm, chan_display(idx, v), chan_unit_str(idx));
    }
}

static void update_ui(float val, float min, float max, uint32_t color_hex) {
    // Ring indicator: only update color based on gauge value
    static uint32_t prev_color = 0;
    if (color_hex != prev_color) {
        lv_obj_set_style_border_color(bar, lv_color_hex(color_hex), 0);
        prev_color = color_hex;
    }

    // Peak high / low labels — only call set_text when content actually changes;
    // lv_label_set_text always invalidates regardless of whether text changed.
    static char prev_high[32] = "";
    static char prev_low[32] = "";
    static uint8_t prev_label_mode = 255; // 0=hidden, 1=peak, 2=secondary

    if (peak_hold_enabled) {
        if (prev_label_mode != 1) {
            lv_obj_clear_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_clear_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 1;
            prev_high[0] = '\0'; prev_low[0] = '\0'; // force text refresh
        }
        char buf[32];
        if (peak_val > -998.0f) snprintf(buf, sizeof(buf), LV_SYMBOL_UP " %.1f", peak_val);
        else                    snprintf(buf, sizeof(buf), LV_SYMBOL_UP " --");
        if (strcmp(buf, prev_high) != 0) {
            lv_label_set_text(peak_high_label, buf);
            strncpy(prev_high, buf, sizeof(prev_high));
        }
        if (peak_low_val < 998.0f) snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " %.1f", peak_low_val);
        else                       snprintf(buf, sizeof(buf), LV_SYMBOL_DOWN " --");
        if (strcmp(buf, prev_low) != 0) {
            lv_label_set_text(peak_low_label, buf);
            strncpy(prev_low, buf, sizeof(prev_low));
        }
    } else if (secondary_chan != 0) {
        char buf[28];
        format_secondary(secondary_chan, buf, sizeof(buf));
        if (prev_label_mode != 2) {
            lv_obj_set_style_text_align(peak_high_label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_align(peak_high_label, LV_ALIGN_CENTER, 0, -120);
            lv_obj_clear_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 2;
            prev_high[0] = '\0';
        }
        if (strcmp(buf, prev_high) != 0) {
            lv_label_set_text(peak_high_label, buf);
            strncpy(prev_high, buf, sizeof(prev_high));
        }
    } else {
        if (prev_label_mode != 0) {
            lv_obj_add_flag(peak_high_label, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(peak_low_label, LV_OBJ_FLAG_HIDDEN);
            prev_label_mode = 0;
        }
    }

    // Update needle tip position based on gauge value
    float angle_start = 135.0f;  // Start angle in degrees (SSW - South-Southwest)
    float angle_range = 270.0f;  // 315 degrees clockwise sweep
    // Path: SSW (112.5°) → South (90°) → N (270°) → E (0°/360°) → SSE (67.5°)
    // Midpoint: 112.5 + 157.5 = 270° (North - 12 o'clock)

    // Map value to angle
    float normalized = (val - min) / (max - min);
    normalized = (normalized < 0) ? 0 : (normalized > 1) ? 1 : normalized;  // Clamp to 0-1
    float angle_deg = angle_start + normalized * angle_range;

    // Convert to radians
    float angle_rad = angle_deg * M_PI / 180.0f;

    // Calculate needle line from 185 to 225px radius to inside of ring
    int center_x = 240;
    int center_y = 240;
    int radius_start = 185;  // Starting radius
    int radius_end = 225;    // Ending radius (tip)

    static lv_point_precise_t needle_points[2];
    static lv_point_precise_t prev_needle[2] = {{-9999,-9999},{-9999,-9999}};

    needle_points[0].x = center_x + (int)(radius_start * cosf(angle_rad));
    needle_points[0].y = center_y + (int)(radius_start * sinf(angle_rad));
    needle_points[1].x = center_x + (int)(radius_end   * cosf(angle_rad));
    needle_points[1].y = center_y + (int)(radius_end   * sinf(angle_rad));

    if (needle_points[0].x != prev_needle[0].x || needle_points[0].y != prev_needle[0].y ||
        needle_points[1].x != prev_needle[1].x || needle_points[1].y != prev_needle[1].y) {
        lv_line_set_points(needle_tip, needle_points, 2);
        memcpy(prev_needle, needle_points, sizeof(needle_points));
    }
}

void update_gauge_master() {
    if (text_color != current_applied_text) {
        lv_obj_set_style_text_color(val_label_int, lv_color_hex(text_color), 0);
        lv_obj_set_style_text_color(val_label_dec, lv_color_hex(text_color), 0);
        current_applied_text = text_color;
    }

    // The active mode reads its bound registry channel, converted to the
    // configured display units (ranges/zones are entered in display units too).
    const ModeConfig& mc0 = behavior.mode[current_mode];
    int chan_idx = chan_index_from_key(mc0.chan_key);
    target_val = chan_display(chan_idx, haltech_value(chan_idx));

    // On a live mode change, jump straight to the new metric instead of sweeping.
    if (snap_displayed) { displayed_val = target_val; snap_displayed = false; }

    // Time-aware smoothing with a per-frame clamp to avoid large jumps.
    // Dynamics come from the behavior config (web-editable, fleet-syncable).
    static unsigned long last_update_ms = 0;
    unsigned long now_ms = millis();
    float dt = last_update_ms ? (now_ms - last_update_ms) / 1000.0f : (1.0f/30.0f);
    last_update_ms = now_ms;
    float delta = target_val - displayed_val;
    if (fabsf(delta) < 0.05f) {
      displayed_val = target_val;
    } else {
      float step = delta * behavior.smoothing;      // lower = smoother/slower
      float max_step = behavior.max_rate * dt;      // units/sec clamp
      if (fabsf(step) > max_step) step = (step > 0) ? max_step : -max_step;
      displayed_val += step;
    }

    if (peak_hold_enabled) {
        if (target_val > peak_val) { peak_val = target_val; peak_timer = millis(); }
        if (target_val < peak_low_val) peak_low_val = target_val;
        if (millis() - peak_timer > behavior.peak_hold_ms) { peak_val = target_val; peak_low_val = target_val; }
    }

    // Generic colour zones from config: v<z1 low, v<z2 mid, else high.
    // Zones outside [min,max] give always-one-colour (old WATER/OIL behavior).
    const ModeConfig& mc = behavior.mode[current_mode];
    uint32_t color_hex;
    if (displayed_val < mc.z1)      color_hex = color_low;
    else if (displayed_val < mc.z2) color_hex = color_mid;
    else                            color_hex = color_high;

    int i_part = (int)displayed_val;
    int d_part = abs((int)((displayed_val - i_part) * 10));
    char b1[16]; snprintf(b1, sizeof(b1), "%d", i_part);
    char b2[16]; snprintf(b2, sizeof(b2), ".%d", d_part);
    static char prev_b1[16] = ""; static char prev_b2[16] = "";
    bool text_changed = false;
    if (strcmp(prev_b1, b1) != 0) {
      lv_label_set_text(val_label_int, b1);
      strncpy(prev_b1, b1, sizeof(prev_b1));
      text_changed = true;
    }
    if (strcmp(prev_b2, b2) != 0) {
      lv_label_set_text(val_label_dec, b2);
      strncpy(prev_b2, b2, sizeof(prev_b2));
      text_changed = true;
    }
    if (text_changed) {
      // Measure the new strings directly with the font — same result as the old
      // lv_obj_update_layout()+lv_obj_get_width() (label width == text width for
      // auto-sized labels) but without forcing a full layout pass per value tick,
      // and it can never read a stale cached width.
      lv_point_t sz;
      lv_text_get_size(&sz, b1, lv_obj_get_style_text_font(val_label_int, LV_PART_MAIN),
                       lv_obj_get_style_text_letter_space(val_label_int, LV_PART_MAIN),
                       0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int int_w = sz.x;
      lv_text_get_size(&sz, b2, lv_obj_get_style_text_font(val_label_dec, LV_PART_MAIN),
                       lv_obj_get_style_text_letter_space(val_label_dec, LV_PART_MAIN),
                       0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
      int dec_w = sz.x;
      // Fixed anchor: integer right-edge and decimal left-edge both at screen_centre + ANCHOR_OFS
      const int ANCHOR_OFS = 44;
      lv_obj_align(val_label_int, LV_ALIGN_CENTER, ANCHOR_OFS - int_w / 2, 5);
      lv_obj_align(val_label_dec, LV_ALIGN_CENTER, ANCHOR_OFS + dec_w / 2, 5);
    }

    update_ui(displayed_val, mc.min, mc.max, color_hex);
}
