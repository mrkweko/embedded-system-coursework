// access.h - Member 2 (RFID access + access hours)
#pragma once
#include <stdbool.h>

void access_start(void);                              // call once from app_main()

// Used by Member 5 (admin commands) and Member 4 (status display)
bool access_is_open(void);                            // is the building open right now?
void access_set_hours(int open_min, int close_min);   // minutes after midnight, e.g. 7*60
void access_get_hours(int *open_min, int *close_min);
void access_set_emergency(bool on);                   // lets people EXIT outside hours