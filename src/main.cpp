// Ultimate Gauge Board — entry point. Just setup() + loop(); everything else
// lives in focused modules:
//   app_state      shared types + live globals (ownership rules documented there)
//   config_store   sole NVS owner
//   themes         theme slots + cross-task staging
//   fleet          ESP-NOW peer sync
//   can_rx         TWAI receive task
//   haltech_decode CAN decode task + snapshot access
//   ui/gauge_page, ui/glowcraft_page   the two screens
//   web/web_server WiFi AP + HTTP + OTA
#include <Arduino.h>
#include "app_state.h"
#include "config_store.h"
#include "themes.h"
#include "fleet.h"
#include "can_rx.h"
#include "haltech_decode.h"
#include "ui/render_shared.h"
#include "ui/gauge_page.h"
#include "ui/glowcraft_page.h"
#include "web/web_server.h"
#include "CANBus_Driver.h"
#include "GlowCraft_Driver.h"
#include "LVGL_Driver.h"
#include "I2C_Driver.h"
#include "Display_ST7701.h"
#include "TCA9554PWR.h"
#include <esp_system.h>

static unsigned long debug_last_print = 0;
#define DEBUG_INTERVAL_MS 5000  // print every 5s when debug enabled
static unsigned long last_data_time = 0;
static unsigned long last_broadcast = 0;

static const char* reset_reason_str(esp_reset_reason_t r) {
  switch (r) {
    case ESP_RST_POWERON:   return "POWER_ON";
    case ESP_RST_SW:        return "SOFTWARE (ESP.restart)";
    case ESP_RST_PANIC:     return "PANIC/EXCEPTION";
    case ESP_RST_INT_WDT:   return "INTERRUPT_WATCHDOG";
    case ESP_RST_TASK_WDT:  return "TASK_WATCHDOG";
    case ESP_RST_WDT:       return "OTHER_WATCHDOG";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_SDIO:      return "SDIO";
    default:                return "UNKNOWN";
  }
}

static void drivers_init() {
  i2c_init(); tca9554pwr_init(0x00); lcd_init(); canbus_init(); glowcraft_init(); lvgl_init();
}

void setup() {
  Serial.begin(115200);
  // Always log reset reason — most useful single diagnostic
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("\n\n=== BOOT: %s (fw v%s, %s) ===\n", reset_reason_str(rr), FIRMWARE_VERSION, FIRMWARE_BUILD);
  Serial.printf("Heap: %u free / %u total\n", ESP.getFreeHeap(), ESP.getHeapSize());
  Serial.printf("PSRAM: %u free / %u total\n", ESP.getFreePsram(), ESP.getPsramSize());

  cfg_set_loop_task();   // setup()/loop() run in loopTask — the only NVS writer

  // After a brownout the ST7701's internal LDOs may still be settling.
  // Hold RESET low longer and wait before initialising to guarantee a clean panel state.
  bool was_brownout = (rr == ESP_RST_BROWNOUT);
  if (was_brownout) {
    Serial.println("[DISP] Brownout detected — extending display reset hold");
    i2c_init();
    tca9554pwr_init(0x00);
    set_exio(EXIO_PIN1, Low);   // Hold RESET low for 300 ms instead of 50 ms
    vTaskDelay(pdMS_TO_TICKS(300));
    set_exio(EXIO_PIN1, High);
    vTaskDelay(pdMS_TO_TICKS(500)); // Extra settle time for panel LDOs
  }

  drivers_init();
  lv_obj_set_style_bg_color(lv_scr_act(), lv_color_black(), 0);

  // Generate unique default name from eFuse chip ID (no WiFi required)
  uint64_t chipid = ESP.getEfuseMac();
  char defaultName[20];
  snprintf(defaultName, sizeof(defaultName), "Gauge-%04X", (uint16_t)(chipid >> 32));

  bool last_boot_completed = cfg_load_all(defaultName);
  cfg_load_behavior(&behavior);   // per-mode ranges/zones + dynamics (defaults = legacy behavior)

  // Build the four theme slots (slot 0 = the legacy theme just loaded above),
  // then make the saved active slot live before the first style pass.
  load_all_themes();
  theme_to_globals(active_theme);

  // --- Crash-safe gradient guard (auto-unbrick) ---
  // A heap-heavy gradient (radial/conical) can fail to render and crash before
  // the panel ever lights. Because that happens before loop() runs, OTA can't
  // recover it. If the last boot didn't complete, disable the active slot's
  // gradient and persist it so we boot clean instead of crash-looping.
  if (!last_boot_completed) {
    Serial.println("[SAFE] Previous boot did not complete — disabling gradient (safe mode)");
    bg_grad_type = 0;
    themes[active_theme].bg_grad_type = 0;
    cfg_disable_gradient(active_theme);
  }
  // Mark boot as in-progress; cleared at the end of setup() once we've rendered
  // and lit the backlight. A crash before then leaves this false -> safe mode.
  cfg_mark_boot_started();

  gauge_scr = lv_scr_act();   // the default screen holds the gauge UI
  setup_wifi();               // bring up AP + web server + OTA BEFORE any risky
                              // rendering so a bad style can never lock out OTA
  load_current_style();
  build_glowcraft_page();
  apply_page();               // load whichever page was last selected

  can_tasks_start();

  // Render a few frames before enabling backlight — ensures the framebuffer
  // contains valid content before it's visible, preventing startup corruption.
  // Pump OTA/web here too so recovery stays possible even if a render stalls.
  for (int i = 0; i < 5; i++) { lv_timer_handler(); web_pump(); }

  // Backlight soft-start: ramp instead of stepping straight to target. A 0->max
  // jump makes the LED boost converter draw a hard inrush right as WiFi beacons
  // + PSRAM render traffic peak — measured brownouts on bench/USB supplies at
  // exactly this moment. ~300ms ramp keeps the rail alive; frames keep pumping.
  for (int b = 5; b < current_brightness; b += 5) {
    set_backlight(b);
    lv_timer_handler();
    vTaskDelay(pdMS_TO_TICKS(15));
  }
  set_backlight(current_brightness);

  // Boot fully succeeded (rendered + backlit) — clear the in-progress flag so the
  // next boot is treated as clean. If we'd crashed above, this stays false.
  cfg_mark_boot_ok();

  // Post-setup memory diagnostics — internal heap is the scarce resource
  // (WiFi + LVGL draw buffers + lwip all draw on it). Watch this on bench.
  Serial.printf("[BOOT] Setup complete. Internal heap: %u free (min ever %u, largest block %u). PSRAM: %u free\n",
                heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                ESP.getFreePsram());
}

void loop() {
  unsigned long lvgl_t = millis();
  lv_timer_handler();
  if (show_perf_stats) perf_lvgl_ms = millis() - lvgl_t;
  web_pump();

  // --- FLAG HANDLERS (loopTask owns globals/LVGL/NVS — everything lands here) ---
  if (reboot_at_ms && (int32_t)(millis() - reboot_at_ms) >= 0) ESP.restart();

  // Staged theme changes from other tasks (ESP-NOW colours, trimpot slot switch):
  // applied to globals + slots + NVS here, then flag_theme_update repaints.
  themes_process_pending();

  // Behavior config pushed in via fleet CONFIG_SYNC: apply + persist here.
  {
    BehaviorConfig bc; uint8_t units;
    if (fleet_take_pending_config(&bc, &units)) {
        behavior = bc;
        cfg_persist_behavior(behavior);
        if (units != 0xFF) {   // v2.1.0 senders include display-unit prefs
            units_press_psi  = units & 1;  cfg_put_bool("u_psi",  units_press_psi);
            units_temp_f     = units & 2;  cfg_put_bool("u_degf", units_temp_f);
            units_speed_mph  = units & 4;  cfg_put_bool("u_mph",  units_speed_mph);
            units_lambda_afr = units & 8;  cfg_put_bool("u_afr",  units_lambda_afr);
        }
        lv_label_set_text(mode_label, behavior.mode[current_mode].label);
        snap_displayed = true;   // active channel/range may have changed — snap, don't sweep
    }
  }

  // Fleet "identify": blink the backlight until the deadline, then restore.
  if (identify_end_ms) {
      if ((int32_t)(millis() - identify_end_ms) >= 0) {
          identify_end_ms = 0;
          set_backlight(current_brightness);
      } else {
          set_backlight(((millis() / 150) & 1) ? current_brightness : 10);
      }
  }

  if (flag_theme_update) {
      flag_theme_update = false;
      apply_theme_colors();   // in-place colour/gradient/font update (no teardown)
  }
  if (flag_bright_update) {
      flag_bright_update = false;
      if (pending_brightness >= 0) {  // remote (ESP-NOW) change: apply + persist here
          current_brightness = pending_brightness;
          pending_brightness = -1;
          cfg_put_int("bright", current_brightness);
      }
      set_backlight(current_brightness);
  }
  if (flag_new_peer) {
      flag_new_peer = false;
      lv_obj_clear_flag(link_icon, LV_OBJ_FLAG_HIDDEN);
  }
  if (flag_stats_update) {
      flag_stats_update = false;
      if(show_perf_stats) lv_obj_clear_flag(perf_label, LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(perf_label, LV_OBJ_FLAG_HIDDEN);
  }
  if (flag_page_update) {
      flag_page_update = false;
      apply_page();
  }
  if (flag_mode_update) {
      flag_mode_update = false;
      if (pending_mode >= 0) {  // remote (ESP-NOW) change: apply + persist here
          current_mode = (GaugeMode)pending_mode;
          pending_mode = -1;
          cfg_put_int("mode", (int)current_mode);
      }
      lv_label_set_text(mode_label, behavior.mode[current_mode].label);
      // Fresh state for the new metric: drop stale peaks and snap the needle so
      // it doesn't sweep across the dial from the old mode's value.
      peak_val = -999.0f; peak_low_val = 999.0f; peak_timer = millis();
      snap_displayed = true;
  }

  // --- STATS LOGIC ---
  if (show_perf_stats) {
      perf_frames++;
      if (millis() - perf_last_time >= 1000) {
          perf_fps = perf_frames;
          perf_frames = 0;
          perf_last_time = millis();
          lv_label_set_text_fmt(perf_label, "FPS:%d LV:%d R:%d UI:%d", perf_fps, perf_lvgl_ms, lvgl_render_ms, perf_frame_ms);
      }
  }

  if (millis() - last_broadcast > 2000) {
      last_broadcast = millis();
      broadcast_presence();
  }

  if (debug_mode_enabled && millis() - debug_last_print > DEBUG_INTERVAL_MS) {
      debug_last_print = millis();
      twai_status_info_t can_status;
      twai_get_status_info(&can_status);
      Serial.printf("[DBG] Heap:%u PSRAM:%u | CAN state:%d tx_err:%u rx_err:%u | Mode:%s Val:%.2f\n",
          ESP.getFreeHeap(),
          ESP.getFreePsram(),
          (int)can_status.state,
          can_status.tx_error_counter,
          can_status.rx_error_counter,
          behavior.mode[current_mode].label,
          displayed_val);
      Serial.printf("[DBG] Loop stack high-water: %u\n", uxTaskGetStackHighWaterMark(NULL));
  }

  if (millis() - last_data_time > 16) {
      unsigned long start = millis();
      last_data_time = start;
      if (current_page == PAGE_GLOWCRAFT) {
          if (test_mode_enabled) glowcraft_test_inject();
          update_glowcraft_page();
      } else {
          if (test_mode_enabled) haltech_test_inject();
          update_gauge_master();
      }

      if(show_perf_stats) perf_frame_ms = millis() - start;
  }
  yield();
}
