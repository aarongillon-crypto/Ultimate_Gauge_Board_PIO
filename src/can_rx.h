// TWAI receive task: drains the hardware RX queue into canMsgQueue for the
// decode task, and drives bus recovery. Owns queue + task creation.
#pragma once
#include <Arduino.h>

// Create canMsgQueue and start RxCAN + ProcCAN (both core 1, prio 2).
void can_tasks_start();
