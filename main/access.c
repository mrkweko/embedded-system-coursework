// access.c - Member 2: decides who may enter or leave, drives the doors, enforces the access hours.
#include "access.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/gpio.h"
#include "console.h"                    // scan_event_t, scan_queue
#include "shared.h"                     // user_t, g_users, g_n_users, g_users_mutex

void rfid_start(void);                  // in rfid.c: starts the two RFID readers

#define PIN_ENTRY_DOOR  GPIO_NUM_25     // green LED  = entry door unlocked (must match diagram.json)
#define PIN_EXIT_DOOR   GPIO_NUM_26     // yellow LED = exit door unlocked

// ---------- PART 1: access hours (my data) ----------
static SemaphoreHandle_t s_mutex;       // protects the three values below
static int  s_open_min  = 7 * 60;       // opens 07:00 (minutes after midnight)
static int  s_close_min = 20 * 60;      // closes 20:00
static bool s_emergency = false;        // true = people may EXIT outside hours

// Is it open now? Works for 07:00-20:00 and for hours that pass midnight. Caller holds the mutex.
static bool in_window(void)
{
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    int m = t.tm_hour * 60 + t.tm_min;
    if (s_open_min < s_close_min) {
        return m >= s_open_min && m < s_close_min;
    }
    return m >= s_open_min || m < s_close_min;
}

bool access_is_open(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool open = in_window();
    xSemaphoreGive(s_mutex);
    return open;
}

int access_schedule_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool open = in_window();
    bool emergency = s_emergency;
    xSemaphoreGive(s_mutex);
    if (open) {
        return ACCESS_OPEN;
    }
    return emergency ? ACCESS_EMERGENCY : ACCESS_CLOSED;
}

void access_set_hours(int open_min, int close_min)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_open_min = open_min;
    s_close_min = close_min;
    xSemaphoreGive(s_mutex);
}

void access_get_hours(int *open_min, int *close_min)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *open_min = s_open_min;
    *close_min = s_close_min;
    xSemaphoreGive(s_mutex);
}

void access_set_emergency(bool on)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_emergency = on;
    xSemaphoreGive(s_mutex);
}

// ---------- PART 2: test users and clock ----------
// Seeds the shared table with Wokwi's built-in cards, only if the table is empty
// (so users loaded from flash by Member 3 are not duplicated).
static void seed_test_users(void)
{
    static const struct { const char *uid, *name; } seed[] = {
        { "01020304", "Alice"  },       // Blue card
        { "11223344", "Brian"  },       // Green card
        { "55667788", "Cathy"  },       // Yellow card
        { "AABBCCDD", "David"  },       // Red card
        { "C0FFEE99", "Esther" },       // Key fob
    };
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    if (g_n_users == 0) {
        for (int i = 0; i < (int)(sizeof(seed) / sizeof(seed[0])) && g_n_users < MAX_USERS; i++) {
            user_t *u = &g_users[g_n_users++];
            strlcpy(u->uid, seed[i].uid, sizeof(u->uid));
            strlcpy(u->name, seed[i].name, sizeof(u->name));
            u->inside = false;
        }
    }
    xSemaphoreGive(g_users_mutex);
}

// The simulation has no clock: start on 2 Nov 2026, 08:00 (Kampala) unless already set.
static void tmp_clock_init(void)
{
    setenv("TZ", "EAT-3", 1);
    tzset();
    if (time(NULL) > 1700000000) {
        return;
    }
    struct tm t = { .tm_year = 2026 - 1900, .tm_mon = 10, .tm_mday = 2, .tm_hour = 8 };
    struct timeval tv = { .tv_sec = mktime(&t), .tv_usec = 0 };
    settimeofday(&tv, NULL);
}

// ---------- PART 3: handling one scan (the core of my task) ----------
static void handle_scan(const scan_event_t *ev)
{
    const char *door = ev->entry ? "ENTRY" : "EXIT";
    const char *denied = NULL;          // the reason, or NULL if access is granted
    char name[16] = "?";

    // 1. Read the access rules (one lock)
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool open = in_window();
    bool emergency = s_emergency;
    xSemaphoreGive(s_mutex);

    // 2. Decide, and update the user's 'inside' flag (another lock)
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    user_t *u = NULL;
    for (int i = 0; i < g_n_users; i++) {
        if (strcmp(g_users[i].uid, ev->uid) == 0) {
            u = &g_users[i];
            break;
        }
    }
    if (u == NULL) {
        denied = "UNKNOWN TAG";
    } else {
        strlcpy(name, u->name, sizeof(name));
        if (!open && !(emergency && !ev->entry)) {
            denied = "OUTSIDE HOURS";               // closed: only an emergency EXIT is allowed
        } else if (ev->entry == u->inside) {        // entering while inside, or leaving while outside
            denied = ev->entry ? "ALREADY INSIDE" : "NOT INSIDE";
        } else {
            u->inside = ev->entry;
        }
    }
    xSemaphoreGive(g_users_mutex);

    // 3. Slow things (printing, the door delay) happen after unlocking
    if (denied != NULL) {
        printf("\n[ACCESS] %s DENIED %s (%s): %s\n", door, name, ev->uid, denied);
        return;
    }
    printf("\n[ACCESS] %s GRANTED %s (%s)%s\n", door, name, ev->uid, open ? "" : " [EMERGENCY EXIT]");
    gpio_num_t pin = ev->entry ? PIN_ENTRY_DOOR : PIN_EXIT_DOOR;
    gpio_set_level(pin, 1);                 // unlock the door for 2 seconds
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(pin, 0);
}

static void access_task(void *arg)
{
    (void)arg;
    scan_event_t ev;
    for (;;) {
        if (xQueueReceive(scan_queue, &ev, portMAX_DELAY) == pdTRUE) {
            handle_scan(&ev);
        }
    }
}

void access_start(void)
{
    s_mutex = xSemaphoreCreateMutex();
    tmp_clock_init();
    seed_test_users();                  // needs g_users_mutex, created in app_main()

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_ENTRY_DOOR) | (1ULL << PIN_EXIT_DOOR),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&out);

    xTaskCreate(access_task, "access", 4096, NULL, 5, NULL);
    rfid_start();
}