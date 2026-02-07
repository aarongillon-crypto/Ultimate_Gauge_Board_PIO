#include <Arduino.h>
#include "CANBus_Driver.h"
#include "LVGL_Driver.h"
#include "I2C_Driver.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"

// --- CONFIGURATION ---
const bool TEST_MODE = true; 

enum GaugeMode { MODE_BOOST, MODE_AFR };
const GaugeMode FIXED_MODE = MODE_AFR; 

// --- FONTS ---
LV_FONT_DECLARE(dseg14_60); 
LV_IMG_DECLARE(gauge_bg);

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

typedef struct struct_haltech_data {
  float boost_psi;
  float afr_gas;
  int rpm;
} struct_haltech_data;
struct_haltech_data HaltechData;

// SMOOTHING VARIABLES
float displayed_val = 0.0;
float target_val = 0.0;
// SMOOTHING FACTOR (0.1 = Slow/Heavy, 0.5 = Snappy, 1.0 = Instant)
const float SMOOTHING_FACTOR = 0.25; 

unsigned long last_data_time = 0; 
int current_color_state = -1; 

const int SEGMENT_COUNT = 30;
lv_obj_t *main_scr;
lv_obj_t *int_label;
lv_obj_t *dec_label;
lv_obj_t *mode_label;
lv_obj_t *main_arc; 
lv_obj_t *bg_arc;

bool receiving_data = false;
volatile bool data_ready = false;

// SCALE CONSTANTS
const float B_MIN = -15.0;
const float B_MAX = 30.0;
const float A_MIN = 8.0;
const float A_MAX = 22.0;

void drivers_init(void) {
  i2c_init();
  tca9554pwr_init(0x00);
  lcd_init();
  canbus_init();
  lvgl_init();
}

void make_black_dividers(void) {
  for (int i = 0; i < SEGMENT_COUNT; i++) {
    float angle_deg = 135 + (i * (270.0 / (SEGMENT_COUNT - 1)));
    float rad = angle_deg * PI / 180.0;
    
    int r_in = 180;
    int r_out = 260;
    
    int x1 = 240 + (int)(r_in * cos(rad));
    int y1 = 240 + (int)(r_in * sin(rad));
    int x2 = 240 + (int)(r_out * cos(rad));
    int y2 = 240 + (int)(r_out * sin(rad));
    
    lv_obj_t * line = lv_line_create(main_scr);
    static lv_point_precise_t * p_array[SEGMENT_COUNT]; 
    lv_point_precise_t * p = (lv_point_precise_t *)malloc(sizeof(lv_point_precise_t) * 2);
    p[0].x = x1; p[0].y = y1;
    p[1].x = x2; p[1].y = y2;
    
    lv_line_set_points(line, p, 2);
    lv_obj_set_style_line_width(line, 6, 0); 
    lv_obj_set_style_line_color(line, lv_color_black(), 0);
    lv_obj_set_style_line_rounded(line, false, 0);
  }
}

void main_scr_ui(void) {
  main_scr = lv_scr_act();
  lv_obj_set_style_bg_color(main_scr, lv_color_black(), 0);

  // BACKGROUND IMAGE
  lv_obj_t * bg_img = lv_image_create(main_scr);
  lv_image_set_src(bg_img, &gauge_bg);
  lv_obj_center(bg_img);
  lv_obj_set_style_image_opa(bg_img, 100, 0); 

  // BG RING
  bg_arc = lv_arc_create(main_scr);
  lv_obj_set_size(bg_arc, 440, 440);
  lv_arc_set_bg_angles(bg_arc, 135, 135 + 270);
  lv_arc_set_value(bg_arc, 0); 
  lv_obj_center(bg_arc);
  lv_obj_set_style_arc_color(bg_arc, lv_color_make(30,30,30), LV_PART_MAIN);
  lv_obj_set_style_arc_width(bg_arc, 40, LV_PART_MAIN); 
  lv_obj_set_style_arc_rounded(bg_arc, false, LV_PART_MAIN);
  lv_obj_remove_style(bg_arc, NULL, LV_PART_KNOB); 
  lv_obj_set_style_arc_opa(bg_arc, 0, LV_PART_INDICATOR); 

  // ACTIVE RING
  main_arc = lv_arc_create(main_scr);
  lv_obj_set_size(main_arc, 440, 440);
  lv_arc_set_bg_angles(main_arc, 135, 135 + 270);
  lv_arc_set_rotation(main_arc, 0);
  lv_arc_set_value(main_arc, 0); 
  lv_arc_set_range(main_arc, 0, 100);
  lv_obj_center(main_arc);
  
  lv_obj_set_style_arc_color(main_arc, lv_palette_main(LV_PALETTE_BLUE), LV_PART_INDICATOR);
  lv_obj_set_style_arc_width(main_arc, 40, LV_PART_INDICATOR);
  lv_obj_set_style_arc_rounded(main_arc, false, LV_PART_INDICATOR);
  lv_obj_set_style_arc_opa(main_arc, 0, LV_PART_MAIN);
  lv_obj_remove_style(main_arc, NULL, LV_PART_KNOB);

  make_black_dividers();

  // INT LABEL
  int_label = lv_label_create(main_scr);
  lv_obj_set_style_text_color(int_label, lv_color_hex(0xFFD700), 0); // GOLD
  lv_obj_set_style_text_font(int_label, &dseg14_60, 0);
  lv_obj_set_style_transform_zoom(int_label, 384, 0); 
  lv_obj_align(int_label, LV_ALIGN_CENTER, -10, 0); 
  
  // DEC LABEL
  dec_label = lv_label_create(main_scr);
  lv_obj_set_style_text_color(dec_label, lv_color_hex(0xFFD700), 0); // GOLD
  lv_obj_set_style_text_font(dec_label, &dseg14_60, 0); 
  lv_obj_set_style_transform_zoom(dec_label, 384, 0); 
  lv_obj_align(dec_label, LV_ALIGN_CENTER, 10, 0); 
  
  lv_label_set_text(int_label, "0"); 
  lv_label_set_text(dec_label, ".0"); 

  // MODE LABEL
  mode_label = lv_label_create(main_scr);
  lv_obj_set_style_text_color(mode_label, lv_color_make(150,150,150), 0);
  #ifdef LV_FONT_MONTSERRAT_28
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_28, 0);
  #else 
    lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_14, 0);
  #endif
  
  if (FIXED_MODE == MODE_BOOST) lv_label_set_text(mode_label, "BOOST");
  else lv_label_set_text(mode_label, "AFR");
  
  lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -80);
}

void generate_test_data() {
  static float t = 0;
  t += 0.05; // Slower time steps for smoother input
  HaltechData.boost_psi = -15 + (sin(t) + 1) * 22.5; 
  HaltechData.afr_gas = 8 + (sin(t*0.5) + 1) * 7.0; 
  data_ready = true;
}

// SMOOTH UPDATE FUNCTION
void update_gauge_smooth() {
    // 1. Calculate Target
    target_val = (FIXED_MODE == MODE_BOOST) ? HaltechData.boost_psi : HaltechData.afr_gas;
    
    // 2. Interpolate (LERP)
    // displayed_val moves towards target_val by SMOOTHING_FACTOR (25%)
    // This creates an "ease-out" animation automatically
    float diff = target_val - displayed_val;
    
    // Small cutoff to stop jitter when close enough
    if (abs(diff) < 0.05) {
        displayed_val = target_val;
    } else {
        displayed_val += diff * SMOOTHING_FACTOR;
    }

    float min_val = (FIXED_MODE == MODE_BOOST) ? B_MIN : A_MIN;
    float max_val = (FIXED_MODE == MODE_BOOST) ? B_MAX : A_MAX;

    // 3. Render 'displayed_val' (not target_val)
    
    int int_part = (int)displayed_val;
    int dec_part = abs((int)((displayed_val - int_part) * 10)); 
    
    char int_buf[16];
    char dec_buf[16];
    snprintf(int_buf, sizeof(int_buf), "%d", int_part);
    snprintf(dec_buf, sizeof(dec_buf), ".%d", dec_part);
    
    lv_label_set_text(int_label, int_buf);
    lv_label_set_text(dec_label, dec_buf);
    
    lv_obj_update_layout(int_label);
    lv_obj_update_layout(dec_label);
    lv_obj_align(int_label, LV_ALIGN_CENTER, -(lv_obj_get_width(int_label) * 0.75) - 10, 0); 
    lv_obj_align(dec_label, LV_ALIGN_CENTER, (lv_obj_get_width(dec_label) * 0.75) + 10, 0);

    // Arc Update
    float render_val = displayed_val;
    if (render_val < min_val) render_val = min_val;
    if (render_val > max_val) render_val = max_val;
    
    int percent = (int)((render_val - min_val) / (max_val - min_val) * 100);
    lv_arc_set_value(main_arc, percent);

    // Color Logic (based on smoothed value)
    int new_state = 0;
    lv_color_t color;

    if (FIXED_MODE == MODE_BOOST) {
        if (displayed_val <= 0) {
            new_state = 0; color = lv_palette_main(LV_PALETTE_BLUE);
        } else if (displayed_val < 20) {
            new_state = 1; color = lv_palette_main(LV_PALETTE_GREEN);
        } else {
            new_state = 2; color = lv_palette_main(LV_PALETTE_RED);
        }
    } else { // AFR
        if (displayed_val < 11.0 || displayed_val > 16.0) {
            new_state = 2; color = lv_palette_main(LV_PALETTE_RED); 
        } else {
            new_state = 1; color = lv_palette_main(LV_PALETTE_GREEN); 
        }
    }

    if (new_state != current_color_state) {
        current_color_state = new_state;
        lv_obj_set_style_arc_color(main_arc, color, LV_PART_INDICATOR);
    }
}

// --- CAN TASKS ---

uint16_t get_uint16_be(uint8_t *data, int offset) {
    return (data[offset] << 8) | data[offset + 1];
}

void process_can_queue_task(void *arg) {
  twai_message_t message;
  while (1) {
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
      switch (message.identifier) {
        case 0x360: { // RPM/MAP
          HaltechData.rpm = get_uint16_be(message.data, 0);
          uint16_t raw_map = get_uint16_be(message.data, 2);
          float map_kpa = raw_map * 0.1;
          float boost = (map_kpa - 101.3) * 0.145038; 
          HaltechData.boost_psi = boost;
          break;
        }
        case 0x368: { // WIDEBAND
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
  delay(1000); 
  Serial.println("BOOTING SMOOTH GAUGE...");
  
  drivers_init();
  set_backlight(40); 
  main_scr_ui();
  
  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);
  
  Serial.println("SETUP COMPLETE");
}

void loop(void) {
  lv_timer_handler();
  
  // RUN FAST (20ms / 50fps)
  if (millis() - last_data_time > 20) { 
      last_data_time = millis();
      
      // Update Target (if test mode)
      if (TEST_MODE) {
          generate_test_data();
      }
      
      // Always update gauge to smooth animation towards target
      update_gauge_smooth();
  }
  vTaskDelay(pdMS_TO_TICKS(2));
}