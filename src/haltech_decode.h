// Haltech CAN decode task + thread-safe access to the decoded data.
//
// ProcCAN decodes frames into a task-private struct and publishes a snapshot
// copy under a critical section. UI/web readers call haltech_get() once per
// frame/request and read only their local copy — no torn cross-field reads.
#pragma once
#include "app_state.h"

// FreeRTOS task body (pinned in can_tasks_start).
void process_can_queue_task(void *arg);

// Copy the latest published snapshot into *out (safe from any task).
void haltech_get(HaltechData_t *out);

// Test mode: drive the working struct with synthetic sine data and publish.
// Called from loop() when test_mode_enabled (replaces direct HaltechData writes).
void haltech_test_inject();
