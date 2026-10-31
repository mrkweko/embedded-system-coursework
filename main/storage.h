#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#include "freertos/FreeRTOS.h"

#define LOG_MESSAGE_SIZE 80
#define LOG_RING_SIZE 20
#define LOG_QUEUE_LENGTH 32

typedef enum {
    LOG_EVENT_ACCESS_GRANTED = 0,
    LOG_EVENT_ACCESS_DENIED,
    LOG_EVENT_RFID_SCAN,
    LOG_EVENT_SYSTEM,
} log_event_type_t;

typedef struct {
    time_t timestamp;
    TickType_t uptime_ticks;
    log_event_type_t type;
    char message[LOG_MESSAGE_SIZE];
} log_entry_t;

void storage_start(void);
bool log_event(log_event_type_t type, const char *fmt, ...);
int log_get_recent(int n, log_entry_t *out);
uint32_t log_dropped_count(void);
bool log_sd_ok(void);
uint32_t log_entries_count(void);

/* Compatibility hooks used by the existing console module. */
void logger_print_recent(int n);
int logger_dropped_count(void);
