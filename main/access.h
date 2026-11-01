// access.h - Member 2 (RFID access + access hours + security/evacuation data)
#pragma once
#include <stdbool.h>

void access_start(void);                              // call once from app_main()

// Used by Member 5 (admin commands) and Member 4 (status display)
bool access_is_open(void);                            // is the building open right now?
void access_set_hours(int open_min, int close_min);   // minutes after midnight, e.g. 7*60
void access_get_hours(int *open_min, int *close_min);
void access_set_emergency(bool on);                   // lets people EXIT outside hours

enum { ACCESS_CLOSED = 0, ACCESS_OPEN = 1, ACCESS_EMERGENCY = 2 };  // EMERGENCY = closed, exit allowed
int access_schedule_state(void);

// ---- Security monitoring and evacuation data (safe to call while scanning runs) ----
typedef struct {
    long time_s;        // time(NULL) when it happened
    char kind[16];      // "REPEATED_DENY", "INCONSISTENT", "CAPACITY", or a kind logged by others
    char uid[16];
    bool entry;         // true = entry reader
} security_event_t;

int  access_get_security_events(security_event_t *out, int max);  // newest first, returns count
void access_log_security_event(const char *kind, const char *uid, bool entry);  // e.g. cross-sensor alerts
int  access_security_alerts(void);      // total security events recorded
int  access_denied_count(void);         // total denied scans
int  access_inconsistencies(void);      // ALREADY INSIDE / NOT INSIDE count

int  access_inside_count(void);         // people inside now
void access_set_capacity(int max);      // 0 = no limit
void access_evacuation_begin(void);     // snapshot who is inside (call when emergency turns on)
int  access_evacuation_report(void);    // prints who from the snapshot has not exited, returns that count
void access_evacuation_end(void);       // clear the snapshot