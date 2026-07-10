#include "can_rx.h"
#include "haltech_decode.h"
#include "CANBus_Driver.h"
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>

QueueHandle_t canMsgQueue;
#define CAN_QUEUE_LENGTH 32
#define CAN_QUEUE_ITEM_SIZE sizeof(twai_message_t)

static void receive_can_task(void *arg) {
  static unsigned long last_recover_ms = 0;
  while (1) {
    twai_message_t message;
    // Non-blocking drain of everything the TWAI RX queue currently holds, bounded
    // so a flooded bus can't starve the same-core LVGL loop. Previously we pulled
    // a single frame per 1 ms tick, so bursts overflowed the (shallow) hardware RX
    // queue and dropped frames — including 5 Hz ones like Rotary Trim 3 (0x3E4),
    // which made theme-sync miss knob changes.
    int drained = 0;
    while (drained < 24) {
      esp_err_t err = twai_receive(&message, 0);
      if (err == ESP_OK) {
        xQueueSend(canMsgQueue, &message, 0);
        drained++;
        continue;
      }
      if (err != ESP_ERR_TIMEOUT) {
        // Actual bus error (not just an empty queue) — recover, rate-limited to 5s.
        unsigned long now = millis();
        if (now - last_recover_ms > 5000) {
          last_recover_ms = now;
          canbus_recover();
        }
      }
      break;  // queue drained (TIMEOUT) or bus error — done for this tick
    }
    vTaskDelay(pdMS_TO_TICKS(1));  // yield to the LVGL loop (same core, lower prio)
  }
}

void can_tasks_start() {
  canMsgQueue = xQueueCreate(CAN_QUEUE_LENGTH, CAN_QUEUE_ITEM_SIZE);
  xTaskCreatePinnedToCore(receive_can_task, "RxCAN", 4096, NULL, 2, NULL, 1);
  xTaskCreatePinnedToCore(process_can_queue_task, "ProcCAN", 4096, NULL, 2, NULL, 1);
}
