// access.h - Member 2 (RFID access + access hours + security/evacuation data)

#pragma once

#include <stdbool.h>

// Initialize the access control system
void access_start(void);

// Building access status
bool access_is_open(void);

void access_set_hours(int open_min, int close_min);
void access_get_hours(int *open_min, int *close_min);
void access_set_emergency(bool on);

enum {
    ACCESS_CLOSED = 0,
    ACCESS_OPEN = 1,
    ACCESS_EMERGENCY = 2
};

int access_schedule_state(void);

// Security monitoring
int access_security_alerts(void);
int access_denied_count(void);
int access_inconsistencies(void);

// Record a security event
void access_log_security_event(const char *kind,
                               const char *uid,
                               bool entry);

// Building occupancy and capacity
int access_inside_count(void);
void access_set_capacity(int max);

// Emergency evacuation
void access_evacuation_begin(void);
int access_evacuation_report(void);
void access_evacuation_end(void);