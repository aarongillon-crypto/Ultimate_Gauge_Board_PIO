// ... (Previous Includes remain the same) ...
#include <Arduino.h>
#include "CANBus_Driver.h"
#include "LVGL_Driver.h"
#include "I2C_Driver.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"
#include "images/tabby_needle.h"
#include "images/tabby_tick.h"

LV_IMG_DECLARE(tabby_needle);
LV_IMG_DECLARE(tabby_tick);

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

// DATA
typedef struct struct_haltech_data {
  float boost_psi;
  float afr_gas;
  int rpm;
} struct_haltech_data;
struct_haltech_data HaltechData;

// MODES
enum GaugeMode { MODE_BOOST, MODE_AFR };
GaugeMode current_mode = MODE_BOOST;
unsigned long last_cycle_time = 0;

// CONSTANTS
const int SCALE_TICKS_COUNT   = 9;
const int BOOST_MIN = -15;
const int BOOST_MAX = 30;
const int AFR_MIN = 10;
const int AFR_MAX = 20;

lv_obj_t *scale_ticks[SCALE_TICKS_COUNT];
lv_obj_t *main_scr;
lv_obj_t *scale;
lv_obj_t *needle_img;
lv_obj_t *mode_label;
lv_obj_t *value_label;
lv_obj_t *main_arc;

bool receiving_data = false;
volatile bool data_ready = false;
static int previous_scale_value = BOOST_MIN; // Start at min

void drivers_init(void) {
  i2c_init();
  tca9554pwr_init(0x00);
  lcd_init();
  canbus_init();
  lvgl_init();
}

static void set_needle_img_value(void * obj, int32_t v) {
  lv_scale_set_image_needle_value(scale, needle_img, v);
}

void update_visuals(float value) {
  lv_label_set_text_fmt(value_label, "%.1f", value);
  
  lv_color_t color;
  if (current_mode == MODE_BOOST) {
    if (value <= 0) color = lv_palette_main(LV_PALETTE_BLUE);
    else if (value < 20) color = lv_palette_main(LV_PALETTE_GREEN);
    else color = lv_palette_main(LV_PALETTE_RED);
  } else {
    if (value < 11.0 || value > 16.0) color = lv_palette_main(LV_PALETTE_RED);
    else color = lv_palette_main(LV_PALETTE_GREEN);
  }
  lv_obj_set_style_arc_color(main_arc, color, LV_PART_MAIN);
}

void update_scale(void) {
  float target = (current_mode == MODE_BOOST) ? HaltechData.boost_psi : HaltechData.afr_gas;
  
  // Direct set for responsiveness
  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, scale);
  lv_anim_set_values(&anim, previous_scale_value, (int)target);
  lv_anim_set_time(&anim, 100);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)set_needle_img_value);
  lv_anim_start(&anim);
  
  previous_scale_value = (int)target;
  update_visuals(target);
}

void needle_sweep() {
  // Sweep from Min -> Max -> Min
  // We use a long animation to show the full range
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, scale);
  lv_anim_set_values(&a, BOOST_MIN, BOOST_MAX);
  lv_anim_set_time(&a, 1000);
  lv_anim_set_playback_time(&a, 1000);
  lv_anim_set_repeat_count(&a, 1);
  lv_anim_set_exec_cb(&a, (lv_anim_exec_xcb_t)set_needle_img_value);
  lv_anim_start(&a);
}

void switch_mode_ui() {
  if (current_mode == MODE_BOOST) {
    lv_scale_set_range(scale, BOOST_MIN, BOOST_MAX);
    lv_label_set_text(mode_label, "BOOST");
  } else {
    lv_scale_set_range(scale, AFR_MIN, AFR_MAX);
    lv_label_set_text(mode_label, "AFR");
  }
}
void make_scale_ticks(void) {
  for (int i = 0; i < SCALE_TICKS_COUNT; i++) {
    scale_ticks[i] = lv_image_create(main_scr);
    lv_image_set_src(scale_ticks[i], &tabby_tick);
    lv_obj_align(scale_ticks[i], LV_ALIGN_CENTER, 0, 196);
    lv_image_set_pivot(scale_ticks[i], 14, -182);
    // Adjusted rotation logic for 270 degree scale
    int rotation_angle = (i * (270 / (SCALE_TICKS_COUNT - 1))) * 10; 
    lv_image_set_rotation(scale_ticks[i], rotation_angle);
  }
}

void main_scr_ui(void) {
  scale = lv_scale_create(main_scr);
  lv_obj_set_size(scale, 480, 480);
  lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
  lv_obj_center(scale);
  lv_scale_set_range(scale, BOOST_MIN, BOOST_MAX);
  
  // ROTATION FIX: 270 degrees total, 135 degrees offset puts gap at bottom
  lv_scale_set_angle_range(scale, 270);
  lv_scale_set_rotation(scale, 135); 
  lv_obj_set_style_bg_opa(scale, LV_OPA_TRANSP, 0);

  // Background Arc
  lv_obj_t *bg_arc = lv_arc_create(main_scr);
  lv_obj_set_size(bg_arc, 420, 420);
  lv_arc_set_bg_angles(bg_arc, 135, 135 + 270);
  lv_arc_set_value(bg_arc, 0);
  lv_obj_center(bg_arc);
  lv_obj_set_style_arc_color(bg_arc, lv_color_make(30,30,30), LV_PART_MAIN);
  lv_obj_set_style_arc_width(bg_arc, 20, LV_PART_MAIN);
  lv_obj_remove_style(bg_arc, NULL, LV_PART_KNOB);

  // Active Arc
  main_arc = lv_arc_create(main_scr);
  lv_obj_set_size(main_arc, 420, 420);
  lv_arc_set_bg_angles(main_arc, 135, 135 + 270);
  lv_arc_set_value(main_arc, 100);
  lv_obj_center(main_arc);
  lv_obj_set_style_arc_color(main_arc, lv_palette_main(LV_PALETTE_BLUE), LV_PART_MAIN);
  lv_obj_set_style_arc_width(main_arc, 20, LV_PART_MAIN);
  lv_obj_remove_style(main_arc, NULL, LV_PART_KNOB);

  make_scale_ticks();

  // Needle
  needle_img = lv_image_create(scale);
  lv_image_set_src(needle_img, &tabby_needle);
  lv_obj_align(needle_img, LV_ALIGN_CENTER, 108 - 40, 0);
  lv_image_set_pivot(needle_img, 40, 36);

  // Digital Value - BIG FONT
  value_label = lv_label_create(main_scr);
  lv_obj_set_style_text_color(value_label, lv_color_white(), 0);
  
  // Font selection logic...
  #ifdef LV_FONT_MONTSERRAT_48
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_48, 0);
  #else
    lv_obj_set_style_text_font(value_label, &lv_font_montserrat_20, 0);
  #endif
  
  lv_label_set_text(value_label, "0.0");

  // --- ZOOM & CENTER FIX ---
  lv_obj_set_style_transform_zoom(value_label, 512, 0);            // 200% Zoom
  lv_obj_set_style_transform_pivot_x(value_label, LV_PCT(50), 0);  // Pivot X: 50%
  lv_obj_set_style_transform_pivot_y(value_label, LV_PCT(50), 0);  // Pivot Y: 50%
  lv_obj_center(value_label);                                      // Re-center

  mode_label = lv_label_create(main_scr);
  lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_set_style_text_color(mode_label, lv_color_make(180,180,180), 0);
  lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_20, 0);
  lv_label_set_text(mode_label, "BOOST");
}

void screens_init(void) {
  main_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(main_scr, lv_color_black(), 0);
  lv_screen_load(main_scr);
  main_scr_ui();
}

uint16_t get_uint16_be(uint8_t *data, int offset) {
    return (data[offset] << 8) | data[offset + 1];
}

void process_can_queue_task(void *arg) {
  twai_message_t message;
  while (1) {
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
      switch (message.identifier) {
        case 0x360: {
          HaltechData.rpm = get_uint16_be(message.data, 0);
          uint16_t raw_map = get_uint16_be(message.data, 2);
          float map_kpa = raw_map * 0.1;
          float boost = (map_kpa - 101.3) * 0.145038; 
          HaltechData.boost_psi = boost;
          break;
        }
        case 0x368: {
          uint16_t raw_lambda = get_uint16_be(message.data, 0);
          HaltechData.afr_gas = (raw_lambda / 1000.0) * 14.7;
          break;
        }
      }
      receiving_data = true;
      data_ready = true;
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void receive_can_task(void *arg) {
  while (1) {
    twai_message_t message;
    if (twai_receive(&message, pdMS_TO_TICKS(5)) == ESP_OK) {
      xQueueSend(canMsgQueue, &message, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

void setup(void) {
  Serial.begin(115200);
  drivers_init();
  set_backlight(80);
  screens_init();
  needle_sweep(); // <--- Watch this on startup!
  
  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);
}

void loop(void) {
  lv_timer_handler();
  if (data_ready) {
    data_ready = false;
    update_scale();
  }
  if (millis() - last_cycle_time > 5000) {
    last_cycle_time = millis();
    if (current_mode == MODE_BOOST) current_mode = MODE_AFR;
    else current_mode = MODE_BOOST;
    switch_mode_ui();
  }
  vTaskDelay(pdMS_TO_TICKS(5));
}