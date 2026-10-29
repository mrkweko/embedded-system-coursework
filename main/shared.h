// shared.h - owned by Member 4. Data shared between tasks. Do not edit without telling Member 4.
#pragma once
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// ---- Shared user table (agreed by Members 2, 4, 5) ----
// ONE table for the whole system. Scans (Member 2), admin commands (Member 5),
// storage (Member 3) and the console (Member 4) all use it.
//
// RULES:
//  1. Always take g_users_mutex before reading or writing g_users / g_n_users.
//  2. Keep the lock short: copy what you need, release, THEN print.
//  3. Only Member 2's scan handler changes the 'inside' flag.
//  4. Card IDs are 8 uppercase hex characters, no colons, e.g. "01020304".

#define MAX_USERS 20

typedef struct {
    char uid[16];     // RFID tag id
    char name[16];    // display name
    bool inside;      // true = currently in the building
} user_t;

extern user_t g_users[MAX_USERS];
extern int g_n_users;
extern SemaphoreHandle_t g_users_mutex;   // created in app_main() before any task starts
