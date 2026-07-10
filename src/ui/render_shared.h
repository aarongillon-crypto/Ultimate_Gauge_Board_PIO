// LVGL object handles shared between the UI pages and loop()'s flag handlers.
// All of these are created and touched ONLY from loopTask.
#pragma once
#include <lvgl.h>

extern lv_obj_t *val_label_int;
extern lv_obj_t *val_label_dec;
extern lv_obj_t *mode_label;
extern lv_obj_t *link_icon;
extern lv_obj_t *bar;
extern lv_obj_t *peak_dot;
extern lv_obj_t *peak_high_label;
extern lv_obj_t *peak_low_label;
extern lv_obj_t *perf_label;
extern lv_obj_t *needle_tip;

extern lv_obj_t *gauge_scr;      // default screen, holds the gauge UI
extern lv_obj_t *glowcraft_scr;  // holds the LED silhouette view
