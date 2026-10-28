// console.h - Member 4 (serial interface)
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// TODO: move this to shared.h once the group agrees on it (Member 4 owns shared.h)
typedef struct {
    char uid[16];   // RFID tag id, e.g. "A1B2C3"
    bool entry;     // true = entry reader, false = exit reader
} scan_event_t;

// Queue the entrance task (Member 2) reads from. Created in main.c.
extern QueueHandle_t scan_queue;

void console_start(void);   // call once from app_main()
