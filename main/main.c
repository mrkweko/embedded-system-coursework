// main.c - Member 4 (Project Owner)
// Starts all system tasks on ONE ESP32

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "console.h"
#include "access.h"
#include "admin.h"
#include "lighting.h"
#include "storage.h"
#include "state.h"

#include "shared.h"
// Queue for simulated RFID scans
QueueHandle_t scan_queue;

user_t g_users[MAX_USERS];
int g_n_users = 0;
SemaphoreHandle_t g_users_mutex;

// Main ESP32 application
void app_main(void)
{
    g_users_mutex = xSemaphoreCreateMutex();
    // Create queue for RFID events
    scan_queue = xQueueCreate(8, sizeof(scan_event_t));

    state_load();
    storage_start();
    state_log_boot();

    // Member 2: before anything that might call access_*()
    access_start();

    // Initialize administrator system
    admin_start();

    // Start administrator monitoring task
    xTaskCreate(admin_monitor_task, "admin_monitor",
                4096, NULL, 2, NULL);

    // Start serial interface
    console_start();

    // Member 1
    lighting_init();
}