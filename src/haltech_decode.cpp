#include "haltech_decode.h"
#include "themes.h"
#include "GlowCraft_Driver.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <math.h>

extern QueueHandle_t canMsgQueue;   // created in can_rx.cpp

// ProcCAN decodes into s_working (task-private), then haltech_publish() copies
// it into s_shared under a short critical section (~60-byte memcpy). Readers
// take a consistent copy via haltech_get(). This replaces the old bare global
// that loopTask read while ProcCAN was mid-write (cross-field torn reads).
static HaltechData_t s_working = {};
static HaltechData_t s_shared  = {};
static portMUX_TYPE  s_haltech_mux = portMUX_INITIALIZER_UNLOCKED;

static void haltech_publish(void) {
  taskENTER_CRITICAL(&s_haltech_mux);
  s_shared = s_working;
  taskEXIT_CRITICAL(&s_haltech_mux);
}

void haltech_get(HaltechData_t *out) {
  taskENTER_CRITICAL(&s_haltech_mux);
  *out = s_shared;
  taskEXIT_CRITICAL(&s_haltech_mux);
}

static uint16_t get_uint16_be(uint8_t *data, int offset) { return (data[offset] << 8) | data[offset + 1]; }

void process_can_queue_task(void *arg) {
  twai_message_t message;
  while (1) {
    bool decoded = false;
    if (xQueueReceive(canMsgQueue, &message, pdMS_TO_TICKS(1)) == pdPASS) {
      decoded = true;
      switch (message.identifier) {
        case 0x360: {
          s_working.rpm          = get_uint16_be(message.data, 0);
          uint16_t raw_map         = get_uint16_be(message.data, 2);
          s_working.boost_psi    = (raw_map * 0.1 - 101.3) * 0.145038;
          uint16_t raw_tps         = get_uint16_be(message.data, 4);
          s_working.tps_percent  = raw_tps / 10;
          uint16_t raw_load        = get_uint16_be(message.data, 6);
          s_working.engine_load_pct = raw_load / 10;
          break;
        }
        case 0x361: {
          uint16_t raw_oil         = get_uint16_be(message.data, 2);
          s_working.oil_press_psi = (raw_oil * 0.1) * 0.145038;
          // Ignition timing: raw = (degrees + 720) * 10, so degrees = raw/10 - 720
          uint16_t raw_ign         = get_uint16_be(message.data, 4);
          s_working.ign_timing_deg = (raw_ign / 10.0f) - 720.0f;
          uint16_t raw_baro        = get_uint16_be(message.data, 6);
          s_working.baro_kpa     = raw_baro * 0.1f;
          break;
        }
        case 0x362: {
          uint16_t raw_coolant     = get_uint16_be(message.data, 0);
          s_working.water_temp_c = (raw_coolant / 10) - 273;
          uint16_t raw_iat         = get_uint16_be(message.data, 2);
          s_working.intake_air_temp_c = (raw_iat / 10) - 273;
          uint16_t raw_fuel_temp   = get_uint16_be(message.data, 4);
          s_working.fuel_temp_c  = (raw_fuel_temp / 10.0f) - 273.0f;
          uint16_t raw_oil_temp    = get_uint16_be(message.data, 6);
          s_working.oil_temp_c   = (raw_oil_temp / 10.0f) - 273.0f;
          break;
        }
        case 0x363: {
          uint16_t raw_fuel        = get_uint16_be(message.data, 0);
          s_working.fuel_press_psi = (raw_fuel * 0.1f) * 0.145038f;
          break;
        }
        case 0x365: {
          uint16_t raw_spd         = get_uint16_be(message.data, 0);
          s_working.vehicle_speed_kph = raw_spd * 0.1f;
          break;
        }
        case 0x366: {
          s_working.gear = (int8_t)message.data[0];
          break;
        }
        case 0x368: {
          uint16_t raw_lambda      = get_uint16_be(message.data, 0);
          s_working.afr_gas      = (raw_lambda / 1000.0) * 14.7;
          break;
        }
        case 0x3E4: {
          // Rotary Trim 3 (byte 6, 4-position rotary returning 0-3). When theme
          // sync is enabled it selects the live colour slot — debounced so only
          // an actual position change stages a switch. The slot activation and
          // ALL global/LVGL work happen in loop() via the theme staging queue
          // (this runs in the CAN task — it must not touch the colour globals).
          if (message.data_length_code > 6) {
            int pos = message.data[6];
            if (pos >= 0 && pos < THEME_SLOTS && pos != last_trimpot3) {
              last_trimpot3 = pos;
              if (trimpot_theme_sync) {
                PendingTheme p = {}; p.op = PT_ACTIVATE_SLOT; p.slot = (uint8_t)pos;
                theme_stage(&p);
              }
            }
          }
          decoded = false;  // no HaltechData fields touched
          break;
        }
        default:
          // GlowCraft strip-status frames (0x500+) — decoded in their own module.
          glowcraft_decode(&message);
          decoded = false;
          break;
      }
      if (decoded) haltech_publish();
    }
    vTaskDelay(pdMS_TO_TICKS(1));
  }
}

// Synthetic bench data — same waveforms the loop used to write into the bare
// global; now writes the working struct and publishes like a real decode.
void haltech_test_inject() {
  static float t = 0; t += 0.05;
  s_working.boost_psi = -15 + (sin(t) + 1) * 22.5;
  s_working.afr_gas = 8 + (sin(t*0.5) + 1) * 7.0;
  s_working.water_temp_c = 50 + (int)((sin(t*0.3) + 1) * 35.0);
  s_working.oil_press_psi = 10 + (sin(t*0.7) + 1) * 45.0;
  s_working.intake_air_temp_c  = 25   + (int)((sin(t*0.20) + 1.0) * 22.5);
  s_working.oil_temp_c         = 80.0f + (sin(t*0.15f) + 1.0f) * 20.0f;
  s_working.fuel_temp_c        = 35.0f + (sin(t*0.18f) + 1.0f) * 10.0f;
  s_working.fuel_press_psi     = 40.0f + (sin(t*0.40f) + 1.0f) * 10.0f;
  s_working.tps_percent        = (int)((sin(t*0.60) + 1.0) * 50.0);
  s_working.engine_load_pct    = (int)((sin(t*0.55) + 1.0) * 50.0);
  s_working.ign_timing_deg     = 15.0f + (sin(t*0.30f) + 1.0f) * 10.0f;
  s_working.baro_kpa           = 101.3f;
  s_working.vehicle_speed_kph  = (sin(t*0.10f) + 1.0f) * 80.0f;
  s_working.gear               = (int8_t)((int)((sin(t*0.08) + 1.0) * 3.0) + 1);
  haltech_publish();
}
