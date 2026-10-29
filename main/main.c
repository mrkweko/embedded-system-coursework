// main.c - Member 4 (project owner). Starts every task. Each member adds ONE line here.
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "console.h"
#include "access.h"

QueueHandle_t scan_queue;

// TEMPORARY stand-in for Member 2's access task. Delete once the real one exists.
static void stub_access_task(void *arg)
{
    scan_event_t ev;
    for (;;) {
        if (xQueueReceive(scan_queue, &ev, portMAX_DELAY) == pdTRUE)
            printf("\n[stub access] got scan %s (%s)\n", ev.uid, ev.entry ? "in" : "out");
    }
}

void app_main(void)
{
    scan_queue = xQueueCreate(8, sizeof(scan_event_t));

    xTaskCreate(stub_access_task, "stub_access", 3072, NULL, 4, NULL);
    console_start();
    access_start();   // Member 2
    // rooms_start();    // Member 1
    // storage_start();  // Member 3
    // admin_start();    // Member 5
}
