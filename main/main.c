// main.c - Member 4 (Project Owner)
// Starts all system tasks on ONE ESP32

#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "console.h"
#include "admin.h"

// Queue for simulated RFID scans
QueueHandle_t scan_queue;

// Temporary access task for Member 2
static void stub_access_task(void *arg)
{
    scan_event_t ev;

    for (;;) {
        if (xQueueReceive(scan_queue, &ev, portMAX_DELAY) == pdTRUE) {
            printf("\n[stub access] got scan %s (%s)\n",
                   ev.uid,
                   ev.entry ? "in" : "out");
        }
    }
}

// Main ESP32 application
void app_main(void)
{
    // Create queue for RFID events
    scan_queue = xQueueCreate(8, sizeof(scan_event_t));

    // Start temporary RFID access task
    xTaskCreate(stub_access_task, "stub_access",
                3072, NULL, 4, NULL);

    // Initialize administrator system
    admin_start();

    // Start administrator monitoring task
    xTaskCreate(admin_monitor_task, "admin_monitor",
                2048, NULL, 2, NULL);

    // Start serial interface
    console_start();

    // Other group members' tasks
    // access_start();   // Member 2
    // rooms_start();    // Member 1
    // storage_start();  // Member 3
}