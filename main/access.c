
// access.c - Member 2: decides who may enter or leave, drives the doors, enforces the access hours,
// and provides security-monitoring and evacuation data.
// RULES: never printf while holding a mutex (copy, release, then print).
//        Two locks, never held together: s_mutex (my data) and g_users_mutex (the shared user table).

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
#include "console.h"
#include "shared.h"
#include "storage.h"

void rfid_start(void);

#define PIN_ENTRY_DOOR  GPIO_NUM_25
#define PIN_EXIT_DOOR   GPIO_NUM_26
#define EVENT_RING      16
#define DENY_STREAK_MAX 3

// ---------- PART 1: my data. s_mutex protects all of it ----------

static SemaphoreHandle_t s_mutex;
static int s_open_min = 7 * 60;
static int s_close_min = 20 * 60;
static bool s_emergency;
static int s_capacity;

static int s_denied, s_inconsistent, s_alerts;
static int s_deny_streak[2];

static security_event_t s_events[EVENT_RING];
static int s_ev_next, s_ev_count;

static bool s_evac_active;
static char s_evac_uid[MAX_USERS][16];
static int s_evac_n;


// Read one of my numbers under the lock.
static int get_int(const int *p)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    int v = *p;
    xSemaphoreGive(s_mutex);
    return v;
}


// Copy the shared user table.
static int copy_users(user_t out[MAX_USERS])
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    int n = g_n_users < MAX_USERS ? g_n_users : MAX_USERS;
    memcpy(out, g_users, n * sizeof(user_t));
    xSemaphoreGive(g_users_mutex);
    return n;
}


// Is it open now?
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


// Record one security event. Caller holds s_mutex.
static void add_event_locked(const char *kind, const char *uid, bool entry)
{
    security_event_t *e = &s_events[s_ev_next];

    e->time_s = (long)time(NULL);
    strlcpy(e->kind, kind, sizeof(e->kind));
    strlcpy(e->uid, uid ? uid : "", sizeof(e->uid));
    e->entry = entry;

    s_ev_next = (s_ev_next + 1) % EVENT_RING;

    if (s_ev_count < EVENT_RING) {
        s_ev_count++;
    }

    s_alerts++;
}


// ---------- PART 2: schedule and settings ----------

int access_schedule_state(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool open = in_window();
    bool emergency = s_emergency;

    xSemaphoreGive(s_mutex);

    // CHANGE 1: Emergency status takes priority over normal opening hours.
    return emergency ? ACCESS_EMERGENCY :
           (open ? ACCESS_OPEN : ACCESS_CLOSED);
}


bool access_is_open(void)
{
    return access_schedule_state() == ACCESS_OPEN;
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


void access_set_capacity(int max)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_capacity = max < 0 ? 0 : max;
    xSemaphoreGive(s_mutex);
}


// ---------- PART 3: security data for the dashboard ----------

int access_security_alerts(void)
{
    return get_int(&s_alerts);
}

int access_denied_count(void)
{
    return get_int(&s_denied);
}

int access_inconsistencies(void)
{
    return get_int(&s_inconsistent);
}


void access_log_security_event(const char *kind, const char *uid, bool entry)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    add_event_locked(kind, uid, entry);
    xSemaphoreGive(s_mutex);
}


int access_get_security_events(security_event_t *out, int max)
{
    if (out == NULL || max <= 0) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    int n = s_ev_count < max ? s_ev_count : max;

    for (int i = 0; i < n; i++) {
        out[i] = s_events[(s_ev_next - 1 - i + EVENT_RING) % EVENT_RING];
    }

    xSemaphoreGive(s_mutex);

    return n;
}


// ---------- PART 4: occupancy and evacuation ----------

int access_inside_count(void)
{
    int inside = 0;

    xSemaphoreTake(g_users_mutex, portMAX_DELAY);

    for (int i = 0; i < g_n_users; i++) {
        inside += g_users[i].inside;
    }

    xSemaphoreGive(g_users_mutex);

    return inside;
}


void access_evacuation_begin(void)
{
    user_t users[MAX_USERS];
    int n = copy_users(users);
    int count = 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    for (int i = 0; i < n; i++) {
        if (users[i].inside) {
            strlcpy(s_evac_uid[count++], users[i].uid,
                    sizeof(s_evac_uid[0]));
        }
    }

    s_evac_n = count;
    s_evac_active = true;

    xSemaphoreGive(s_mutex);

    printf("\n[EVACUATION] started: %d person(s) recorded inside\n", count);
}


int access_evacuation_report(void)
{
    char snap[MAX_USERS][16];

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool active = s_evac_active;
    int n = s_evac_n;

    memcpy(snap, s_evac_uid, sizeof(snap));

    xSemaphoreGive(s_mutex);

    if (!active) {
        printf("No evacuation in progress.\n");
        return 0;
    }

    user_t users[MAX_USERS];
    int total = copy_users(users);

    int miss[MAX_USERS];
    int missing = 0;

    for (int i = 0; i < n; i++) {
        for (int j = 0; j < total; j++) {
            if (users[j].inside && strcmp(users[j].uid, snap[i]) == 0) {
                miss[missing++] = j;
            }
        }
    }

    printf("---- EVACUATION REPORT ----\n");

    printf("Recorded inside at start: %d, exited since: %d, still unaccounted for: %d\n",
           n, n - missing, missing);

    for (int i = 0; i < missing; i++) {
        printf("  NOT EXITED: %-12s (%s)\n",
               users[miss[i]].name, users[miss[i]].uid);
    }

    return missing;
}


void access_evacuation_end(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    s_evac_active = false;
    s_evac_n = 0;

    xSemaphoreGive(s_mutex);
}


// ---------- PART 5: test users and clock ----------

// Seeds the shared table with Wokwi's built-in cards.
static void seed_test_users(void)
{
    static const struct {
        const char *uid;
        const char *name;
    } seed[] = {
        { "01020304", "Alice"  },
        { "11223344", "Brian"  },
        { "55667788", "Cathy"  },
        { "AABBCCDD", "David"  },
        { "C0FFEE99", "Esther" }
    };

    xSemaphoreTake(g_users_mutex, portMAX_DELAY);

    if (g_n_users == 0) {
        for (int i = 0; i < 5; i++) {
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

    struct tm t = {
        .tm_year = 2026 - 1900,
        .tm_mon = 10,
        .tm_mday = 2,
        .tm_hour = 8
    };

    struct timeval tv = {
        .tv_sec = mktime(&t),
        .tv_usec = 0
    };

    settimeofday(&tv, NULL);
}


// ---------- PART 6: handling one scan ----------

// Updates denial counters and security events.
static bool note_result(const scan_event_t *ev, const char *denied)
{
    int r = ev->entry ? 0 : 1;
    bool alert = false;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (denied == NULL) {
        s_deny_streak[r] = 0;
    } else {
        s_denied++;

        if (strcmp(denied, "ALREADY INSIDE") == 0 ||
            strcmp(denied, "NOT INSIDE") == 0) {

            s_inconsistent++;
            add_event_locked("INCONSISTENT", ev->uid, ev->entry);

        } else if (strcmp(denied, "BUILDING FULL") == 0) {

            add_event_locked("CAPACITY", ev->uid, ev->entry);
        }

        if (++s_deny_streak[r] >= DENY_STREAK_MAX) {
            s_deny_streak[r] = 0;

            add_event_locked("REPEATED_DENY", ev->uid, ev->entry);
            alert = true;
        }
    }

    xSemaphoreGive(s_mutex);

    return alert;
}


static void handle_scan(const scan_event_t *ev)
{
    const char *door = ev->entry ? "ENTRY" : "EXIT";
    const char *denied = NULL;

    char name[16] = "?";

    // 1. Read access rules.
    xSemaphoreTake(s_mutex, portMAX_DELAY);

    bool open = in_window();
    bool emergency = s_emergency;
    int capacity = s_capacity;

    xSemaphoreGive(s_mutex);

    // 2. Decide access and update the user's inside flag.
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);

    user_t *u = NULL;
    int inside_n = 0;

    for (int i = 0; i < g_n_users; i++) {
        inside_n += g_users[i].inside;

        if (u == NULL && strcmp(g_users[i].uid, ev->uid) == 0) {
            u = &g_users[i];
        }
    }

    if (u == NULL) {
        denied = "UNKNOWN TAG";

    } else {
        strlcpy(name, u->name, sizeof(name));

        // CHANGE 2: Emergency mode must block all new entries.
        if (emergency && ev->entry) {
            denied = "EMERGENCY ACTIVE";

        } else if (!open && !(emergency && !ev->entry)) {
            denied = "OUTSIDE HOURS";

        } else if (ev->entry == u->inside) {
            denied = ev->entry ? "ALREADY INSIDE" : "NOT INSIDE";

        } else if (ev->entry && capacity > 0 && inside_n >= capacity) {
            denied = "BUILDING FULL";

        } else {
            u->inside = ev->entry;
        }
    }

    xSemaphoreGive(g_users_mutex);

    // 3. Record the result.
    bool alert = note_result(ev, denied);

    // 4. Printing, logging and door delay happen after unlocking.
    if (denied != NULL) {

        printf("\n[ACCESS] %s DENIED %s (%s): %s\n",
               door, name, ev->uid, denied);

        log_event(LOG_EVENT_ACCESS_DENIED,
                  "%s DENIED %s (%s): %s",
                  door, name, ev->uid, denied);

        if (alert) {
            printf("[SECURITY] %d denied scans in a row at the %s reader (last card %s)\n",
                   DENY_STREAK_MAX, door, ev->uid);

            log_event(LOG_EVENT_SYSTEM,
                      "SECURITY: %d denied scans in a row at %s reader (card %s)",
                      DENY_STREAK_MAX, door, ev->uid);
        }

        return;
    }

    printf("\n[ACCESS] %s GRANTED %s (%s)%s\n",
           door, name, ev->uid,
           open ? "" : " [EMERGENCY EXIT]");

    log_event(LOG_EVENT_ACCESS_GRANTED,
              "%s GRANTED %s (%s)%s",
              door, name, ev->uid,
              open ? "" : " [EMERGENCY EXIT]");

    gpio_num_t pin = ev->entry ? PIN_ENTRY_DOOR : PIN_EXIT_DOOR;

    gpio_set_level(pin, 1);
    vTaskDelay(pdMS_TO_TICKS(2000));
    gpio_set_level(pin, 0);
}


// Access task.
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


// Initialize access control.
void access_start(void)
{
    s_mutex = xSemaphoreCreateMutex();

    tmp_clock_init();
    seed_test_users();

    gpio_config_t out = {
        .pin_bit_mask = (1ULL << PIN_ENTRY_DOOR) |
                        (1ULL << PIN_EXIT_DOOR),
        .mode = GPIO_MODE_OUTPUT,
    };

    gpio_config(&out);

    xTaskCreate(access_task, "access", 4096, NULL, 5, NULL);

    rfid_start();
}

