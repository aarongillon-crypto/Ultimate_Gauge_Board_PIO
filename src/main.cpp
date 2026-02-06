#include <Arduino.h>
#include "CANBus_Driver.h"
#include "LVGL_Driver.h"
#include "I2C_Driver.h"

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include "freertos/queue.h"

// IMAGES
#include "images/tabby_needle.h"
#include "images/tabby_tick.h"

LV_IMG_DECLARE(tabby_needle);
LV_IMG_DECLARE(tabby_tick);

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

// HALTECH DATA
typedef struct struct_haltech_data {
  float boost_psi;
  float afr_gas;
  int rpm;
} struct_haltech_data;

struct_haltech_data HaltechData;

// MODES
enum GaugeMode {
  MODE_BOOST,
  MODE_AFR
};
GaugeMode current_mode = MODE_BOOST;
unsigned long last_cycle_time = 0;

// CONTROL CONSTANTS
const int AVERAGE_VALUES      = 10;
const int SCALE_TICKS_COUNT   = 9;

// Boost Scale: -15 PSI to 30 PSI
const int BOOST_MIN = -15;
const int BOOST_MAX = 30;

// AFR Scale: 10.0 to 20.0
const int AFR_MIN = 10;
const int AFR_MAX = 20;

lv_obj_t *scale_ticks[SCALE_TICKS_COUNT];
#define TAG "TWAI"

// CONTROL VARIABLES
bool receiving_data           = false;
volatile bool data_ready      = false;
int scale_moving_average      = 0;

// GLOBAL COMPONENTS
lv_obj_t *main_scr;
lv_obj_t *scale;
lv_obj_t *needle_img;
lv_obj_t *mode_label;

void drivers_init(void) {
  i2c_init();
  tca9554pwr_init(0x00);
  lcd_init();
  canbus_init();
  lvgl_init();
}

// Smoothing function
int get_moving_average(int new_value) {
    static int values[AVERAGE_VALUES] = {0};
    static int index = 0;
    static int count = 0;
    static double sum = 0;

    sum -= values[index];
    values[index] = new_value;
    sum += new_value;

    index = (index + 1) % AVERAGE_VALUES;
    if (count < AVERAGE_VALUES) count++;

    return (int)roundf(sum / count);
}

static int previous_scale_value = 0;

static void set_needle_img_value(void * obj, int32_t v) {
  lv_scale_set_image_needle_value(scale, needle_img, v);
}

void update_gauge_visuals() {
  if (current_mode == MODE_BOOST) {
    lv_scale_set_range(scale, BOOST_MIN, BOOST_MAX);
    lv_label_set_text(mode_label, "BOOST (PSI)");
  } else {
    lv_scale_set_range(scale, AFR_MIN, AFR_MAX);
    lv_label_set_text(mode_label, "AFR");
  }
}

void update_scale(void) {
  int target_value = 0;
  
  if (current_mode == MODE_BOOST) {
    target_value = (int)HaltechData.boost_psi;
  } else {
    target_value = (int)HaltechData.afr_gas;
  }

  // Use smoothing
  int averaged_value = get_moving_average(target_value);

  lv_anim_t anim;
  lv_anim_init(&anim);
  lv_anim_set_var(&anim, scale);
  lv_anim_set_values(&anim, previous_scale_value, averaged_value);
  lv_anim_set_time(&anim, 100);
  lv_anim_set_exec_cb(&anim, (lv_anim_exec_xcb_t)set_needle_img_value);
  lv_anim_start(&anim);

  previous_scale_value = averaged_value;
}

void make_scale_ticks(void) {
  for (int i = 0; i < SCALE_TICKS_COUNT; i++) {
    scale_ticks[i] = lv_image_create(main_scr);
    lv_image_set_src(scale_ticks[i], &tabby_tick);
    lv_obj_align(scale_ticks[i], LV_ALIGN_CENTER, 0, 196);
    lv_image_set_pivot(scale_ticks[i], 14, -182);
    int rotation_angle = (((i) * (240 / (SCALE_TICKS_COUNT - 1))) * 10); 
    lv_image_set_rotation(scale_ticks[i], rotation_angle);
  }
}

void main_scr_ui(void) {
  scale = lv_scale_create(main_scr);
  lv_obj_set_size(scale, 480, 480);
  lv_scale_set_mode(scale, LV_SCALE_MODE_ROUND_INNER);
  lv_obj_center(scale);
  lv_scale_set_range(scale, BOOST_MIN, BOOST_MAX);
  lv_scale_set_angle_range(scale, 240);
  lv_scale_set_rotation(scale, 90);

  // Background arcs
  lv_obj_t *lower_arc = lv_arc_create(main_scr);
  lv_obj_set_size(lower_arc, 420, 420);
  lv_arc_set_bg_angles(lower_arc, 90, 330);
  lv_arc_set_value(lower_arc, 0);
  lv_obj_center(lower_arc);
  lv_obj_set_style_arc_color(lower_arc, lv_color_make(87,10,1), LV_PART_MAIN);
  lv_obj_set_style_arc_width(lower_arc, 28, LV_PART_MAIN);
  
  make_scale_ticks();
  
  // Needle
  needle_img = lv_image_create(scale);
  lv_image_set_src(needle_img, &tabby_needle);
  lv_obj_align(needle_img, LV_ALIGN_CENTER, 108 - 40, 0);
  lv_image_set_pivot(needle_img, 40, 36);

  // Label for mode
  mode_label = lv_label_create(main_scr);
  lv_obj_align(mode_label, LV_ALIGN_BOTTOM_MID, 0, -100);
  lv_obj_set_style_text_color(mode_label, lv_color_white(), 0);
  lv_obj_set_style_text_font(mode_label, &lv_font_montserrat_20, 0);
  lv_label_set_text(mode_label, "BOOST (PSI)");
}

void screens_init(void) {
  main_scr = lv_obj_create(NULL);
  lv_obj_set_style_bg_color(main_scr, lv_color_black(), 0);
  lv_screen_load(main_scr);
  main_scr_ui();
}

// HELPER: Haltech is Big Endian
uint16_t get_uint16_be(uint8_t *data, int offset) {
    return (data[offset] << 8) | data[offset + 1];
}

void process_can_queue_task(void *arg) {
  twai_message_t message;
  while (1) {
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
      
      switch (message.identifier) {
        // RPM & MAP
        case 0x360: {
          HaltechData.rpm = get_uint16_be(message.data, 0);
          uint16_t raw_map = get_uint16_be(message.data, 2);
          float map_kpa = raw_map * 0.1;
          // Convert to PSI (Gauge): (kPa - 101.3) * 0.145
          float boost = (map_kpa - 101.3) * 0.145038; 
          HaltechData.boost_psi = boost;
          break;
        }
        // WIDEBAND AFR
        case 0x368: {
          uint16_t raw_lambda = get_uint16_be(message.data, 0);
          // Lambda 1.000 = 14.7 AFR
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

  // CYCLE MODE EVERY 5 SECONDS (Temporary)
  if (millis() - last_cycle_time > 5000) {
    last_cycle_time = millis();
    if (current_mode == MODE_BOOST) current_mode = MODE_AFR;
    else current_mode = MODE_BOOST;
    
    update_gauge_visuals();
  }
  
  vTaskDelay(pdMS_TO_TICKS(5));
}