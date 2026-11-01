#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>
#include <ctype.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "admin.h"
#include "shared.h"
#include "access.h"
#include "state.h"
#include "esp_system.h"
#include "dashboard.h"

/* System settings */
#define DEFAULT_ADMIN_PIN "1234"
#define MAX_LOGIN_ATTEMPTS 3
#define LOCKOUT_SECONDS 60
#define DEFAULT_TIMEOUT 120

/* Administrator information */
static char admin_pin[9] = DEFAULT_ADMIN_PIN;
static bool admin_logged_in = false;
static int failed_attempts = 0;
static bool account_locked = false;

static TickType_t lockout_start = 0;
static TickType_t last_activity = 0;
static unsigned int timeout_seconds = DEFAULT_TIMEOUT;

/* Access schedule */
static int opening_time = 7 * 60;
static int closing_time = 20 * 60;
static int current_time = 8 * 60;

/* Emergency mode */
static bool emergency_active = false;


void admin_state_snapshot(char pin[9],
                          unsigned int *timeout,
                          int *simulated_time)
{
    strlcpy(pin, admin_pin, 9);
    *timeout = timeout_seconds;
    *simulated_time = current_time;
}


static bool convert_time(const char *text, int *minutes)
{
    int hour, minute;
    char extra;

    if (text == NULL || minutes == NULL ||
        sscanf(text, "%d:%d%c", &hour, &minute, &extra) != 2) {
        return false;
    }

    if (hour < 0 || hour > 23 || minute < 0 || minute > 59) {
        return false;
    }

    *minutes = hour * 60 + minute;

    return true;
}


static unsigned int elapsed_seconds(TickType_t start)
{
    return (unsigned int)(
        (xTaskGetTickCount() - start) / configTICK_RATE_HZ
    );
}


static bool is_account_locked(void)
{
    if (!account_locked) {
        return false;
    }

    if (elapsed_seconds(lockout_start) >= LOCKOUT_SECONDS) {
        account_locked = false;
        failed_attempts = 0;

        printf("Account unlocked. You may log in again.\n");

        return false;
    }

    printf("Account is locked. Please wait.\n");

    return true;
}


/* Validate an 8-character hexadecimal UID */
static bool normalize_uid(const char *input, char output[9])
{
    if (input == NULL || strlen(input) != 8) {
        return false;
    }

    for (int i = 0; i < 8; i++) {
        unsigned char c = (unsigned char)input[i];

        if (!isxdigit(c)) {
            return false;
        }

        output[i] = (char)toupper(c);
    }

    output[8] = '\0';

    return true;
}


/* Call only while g_users_mutex is held */
static int find_shared_user_locked(const char *uid)
{
    int count = g_n_users;

    if (count < 0) count = 0;
    if (count > MAX_USERS) count = MAX_USERS;

    for (int i = 0; i < count; i++) {
        if (strcmp(g_users[i].uid, uid) == 0) {
            return i;
        }
    }

    return -1;
}


/* ADMIN LOGIN */
static void handle_login(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: login <PIN>\n");
        return;
    }

    if (is_account_locked()) {
        return;
    }

    if (strcmp(argv[1], admin_pin) == 0) {
        admin_logged_in = true;
        failed_attempts = 0;
        last_activity = xTaskGetTickCount();

        printf("Login successful.\n");
    } else {
        failed_attempts++;

        int remaining = MAX_LOGIN_ATTEMPTS - failed_attempts;

        if (remaining < 0) remaining = 0;

        printf("Incorrect PIN. Attempts remaining: %d\n", remaining);

        if (failed_attempts >= MAX_LOGIN_ATTEMPTS) {
            account_locked = true;
            admin_logged_in = false;
            lockout_start = xTaskGetTickCount();

            printf("Account locked for %d seconds.\n",
                   LOCKOUT_SECONDS);
        }
    }
}


/* ADMIN LOGOUT */
static void handle_logout(void)
{
    if (admin_logged_in) {
        admin_logged_in = false;

        printf("Administrator logged out.\n");
    } else {
        printf("You are not logged in.\n");
    }
}


/* CHANGE ADMIN PIN */
static void handle_pin(int argc, char *argv[])
{
    if (argc != 3 || strcmp(argv[1], "set") != 0) {
        printf("Usage: pin set <newPIN>\n");
        return;
    }

    size_t length = strlen(argv[2]);

    if (length < 4 || length > 8) {
        printf("PIN must contain 4 to 8 digits.\n");
        return;
    }

    for (size_t i = 0; i < length; i++) {
        if (argv[2][i] < '0' || argv[2][i] > '9') {
            printf("PIN must contain digits only.\n");
            return;
        }
    }

    memcpy(admin_pin, argv[2], length + 1);

    state_update_admin(admin_pin, timeout_seconds, current_time);
    state_save();

    printf("Administrator PIN changed successfully.\n");
}


/* ACCESS SCHEDULE */
static void handle_schedule(int argc, char *argv[])
{
    int open, close;

    if (argc != 4 || strcmp(argv[1], "set") != 0) {
        printf("Usage: schedule set <HH:MM> <HH:MM>\n");
        return;
    }

    if (!convert_time(argv[2], &open) ||
        !convert_time(argv[3], &close)) {
        printf("Invalid time. Use HH:MM format.\n");
        return;
    }

    if (open == close) {
        printf("Opening and closing times cannot be equal.\n");
        return;
    }

    opening_time = open;
    closing_time = close;

    access_set_hours(open, close);

    state_update_access(open, close, emergency_active);
    state_save();

    printf("Access schedule updated: %s to %s\n",
           argv[2], argv[3]);
}


/* SIMULATED TIME */
static void handle_time(int argc, char *argv[])
{
    int new_time;

    if (argc != 3 || strcmp(argv[1], "set") != 0) {
        printf("Usage: time set <HH:MM>\n");
        return;
    }

    if (!convert_time(argv[2], &new_time)) {
        printf("Invalid time. Use HH:MM format.\n");
        return;
    }

    current_time = new_time;

    state_update_admin(admin_pin, timeout_seconds, current_time);
    state_save();

    printf("Simulated time updated to %s.\n", argv[2]);
    printf("Note: Use schedule set to test access hours.\n");
}


/* USER MANAGEMENT */
static void handle_user(int argc, char *argv[])
{
    if (argc < 3 || argc > 4) {
        printf("Usage: user add <UID> [NAME]\n");
        printf("   or: user del <UID>\n");
        return;
    }

    char uid[9];

    if (!normalize_uid(argv[2], uid)) {
        printf("UID must be exactly 8 hexadecimal characters.\n");
        return;
    }

    if (strcmp(argv[1], "add") == 0) {

        if (argc == 4 &&
            (argv[3][0] == '\0' ||
             strlen(argv[3]) >= sizeof(g_users[0].name))) {
            printf("Name must contain 1 to 15 characters.\n");
            return;
        }

        const char *name = (argc == 4) ? argv[3] : uid;

        xSemaphoreTake(g_users_mutex, portMAX_DELAY);

        if (g_n_users < 0 || g_n_users > MAX_USERS) {
            xSemaphoreGive(g_users_mutex);
            printf("Shared user count is invalid.\n");
            return;
        }

        if (find_shared_user_locked(uid) >= 0) {
            xSemaphoreGive(g_users_mutex);
            printf("User already exists: %s\n", uid);
            return;
        }

        if (g_n_users >= MAX_USERS) {
            xSemaphoreGive(g_users_mutex);
            printf("User table is full.\n");
            return;
        }

        user_t *user = &g_users[g_n_users];

        memset(user, 0, sizeof(*user));

        strcpy(user->uid, uid);
        strcpy(user->name, name);

        user->inside = false;

        g_n_users++;

        state_mark_dirty();
        state_save_locked();

        xSemaphoreGive(g_users_mutex);

        printf("User registered: %s (%s)\n", uid, name);

    } else if (strcmp(argv[1], "del") == 0) {

        if (argc != 3) {
            printf("Usage: user del <UID>\n");
            return;
        }

        xSemaphoreTake(g_users_mutex, portMAX_DELAY);

        int index = find_shared_user_locked(uid);

        if (index < 0) {
            xSemaphoreGive(g_users_mutex);
            printf("User not found: %s\n", uid);
            return;
        }

        if (g_users[index].inside) {
            xSemaphoreGive(g_users_mutex);
            printf("Cannot delete a user who is currently inside.\n");
            return;
        }

        for (int i = index; i < g_n_users - 1; i++) {
            g_users[i] = g_users[i + 1];
        }

        g_n_users--;

        memset(&g_users[g_n_users], 0, sizeof(g_users[0]));

        state_mark_dirty();
        state_save_locked();

        xSemaphoreGive(g_users_mutex);

        printf("User deleted: %s\n", uid);

    } else {
        printf("Use 'add' or 'del'.\n");
    }
}


/* DISPLAY REGISTERED USERS */
static void handle_users(void)
{
    user_t snapshot[MAX_USERS];
    int count;

    xSemaphoreTake(g_users_mutex, portMAX_DELAY);

    count = g_n_users;

    if (count < 0) count = 0;
    if (count > MAX_USERS) count = MAX_USERS;

    memcpy(snapshot, g_users, (size_t)count * sizeof(user_t));

    xSemaphoreGive(g_users_mutex);

    printf("\n--- REGISTERED USERS (%d) ---\n", count);

    if (count == 0) {
        printf("No registered users.\n");
        return;
    }

    for (int i = 0; i < count; i++) {
        printf("%d. %s | %s | %s\n",
               i + 1,
               snapshot[i].uid,
               snapshot[i].name,
               snapshot[i].inside ? "INSIDE" : "OUTSIDE");
    }
}


/* AUTOMATIC LOGOUT TIMEOUT */
static void handle_timeout(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: timeout <seconds>\n");
        return;
    }

    char *end;

    unsigned long value = strtoul(argv[1], &end, 10);

    if (argv[1][0] == '\0' || *end != '\0' ||
        value < 10 || value > 3600) {
        printf("Timeout must be 10 to 3600 seconds.\n");
        return;
    }

    timeout_seconds = (unsigned int)value;

    state_update_admin(admin_pin, timeout_seconds, current_time);
    state_save();

    printf("Automatic logout set to %u seconds.\n",
           timeout_seconds);
}


/* EMERGENCY MODE */
static void handle_emergency(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: emergency <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {

        if (!emergency_active) {

            emergency_active = true;

            access_evacuation_begin();
            access_set_emergency(true);

            state_update_access(
                opening_time,
                closing_time,
                emergency_active
            );

            state_save();

            printf("EMERGENCY MODE ACTIVATED.\n");
            printf("Evacuation tracking started.\n");

        } else {
            printf("Emergency mode is already active.\n");
        }

    } else if (strcmp(argv[1], "off") == 0) {

        if (emergency_active) {

            emergency_active = false;

            access_set_emergency(false);
            access_evacuation_end();

            state_update_access(
                opening_time,
                closing_time,
                emergency_active
            );

            state_save();

            printf("Emergency mode deactivated.\n");

        } else {
            printf("Emergency mode is already inactive.\n");
        }

    } else {
        printf("Use emergency on or emergency off.\n");
    }
}


/* EVACUATION REPORT */
static void handle_evacuation(void)
{
    if (!emergency_active) {
        printf("No active emergency.\n");
        return;
    }

    int remaining = access_evacuation_report();

    printf("People still unaccounted for: %d\n", remaining);
}


/* DISPLAY ADMIN SETTINGS */
static void handle_settings(void)
{
    printf("\n--- ADMIN SETTINGS ---\n");

    printf("Opening time: %02d:%02d\n",
           opening_time / 60, opening_time % 60);

    printf("Closing time: %02d:%02d\n",
           closing_time / 60, closing_time % 60);

    printf("Simulated time: %02d:%02d\n",
           current_time / 60, current_time % 60);

    printf("Logout timeout: %u seconds\n", timeout_seconds);

    printf("Emergency mode: %s\n",
           emergency_active ? "ON" : "OFF");
}


/* INITIALIZE ADMIN SYSTEM */
esp_err_t admin_start(void)
{
    if (g_users_mutex == NULL) {
        printf("ERROR: g_users_mutex must be created before admin_start().\n");
        return ESP_FAIL;
    }

    admin_logged_in = false;
    failed_attempts = 0;
    account_locked = false;
    emergency_active = false;

    last_activity = xTaskGetTickCount();

    char loaded_pin[9];
    unsigned int loaded_timeout;
    int loaded_time;
    bool loaded_emergency;

    if (state_get_loaded_admin(
            loaded_pin,
            &loaded_timeout,
            &loaded_time,
            &loaded_emergency)) {

        strlcpy(admin_pin, loaded_pin, sizeof(admin_pin));

        timeout_seconds = loaded_timeout;
        current_time = loaded_time;
        emergency_active = loaded_emergency;

        state_get_loaded_access(
            &opening_time,
            &closing_time,
            &loaded_emergency
        );

        /* Synchronize saved settings with access module */
        access_set_hours(opening_time, closing_time);
        access_set_emergency(emergency_active);
    }

    printf("\n=== CoCIS ADMIN INTERFACE ===\n");
    printf("Use login <PIN> to begin.\n");

    return ESP_OK;
}


/* ADMIN COMMAND HANDLER */
bool admin_handle_command(int argc, char *argv[])
{
    if (argc < 1) {
        return false;
    }

    if (strcmp(argv[0], "login") == 0) {
        handle_login(argc, argv);
        return true;
    }

    if (strcmp(argv[0], "logout") == 0) {
        handle_logout();
        return true;
    }

    bool admin_command =
        strcmp(argv[0], "pin") == 0 ||
        strcmp(argv[0], "schedule") == 0 ||
        strcmp(argv[0], "time") == 0 ||
        strcmp(argv[0], "user") == 0 ||
        strcmp(argv[0], "users") == 0 ||
        strcmp(argv[0], "timeout") == 0 ||
        strcmp(argv[0], "emergency") == 0 ||
        strcmp(argv[0], "evacuation") == 0 ||
        strcmp(argv[0], "settings") == 0 ||
        strcmp(argv[0], "dashboard") == 0 ||
        strcmp(argv[0], "reboot") == 0;

    if (!admin_command) {
        return false;
    }

    if (!admin_logged_in) {
        printf("Access denied. Please log in first.\n");
        return true;
    }

    last_activity = xTaskGetTickCount();

    if (strcmp(argv[0], "pin") == 0) {
        handle_pin(argc, argv);

    } else if (strcmp(argv[0], "schedule") == 0) {
        handle_schedule(argc, argv);

    } else if (strcmp(argv[0], "time") == 0) {
        handle_time(argc, argv);

    } else if (strcmp(argv[0], "user") == 0) {
        handle_user(argc, argv);

    } else if (strcmp(argv[0], "users") == 0) {
        handle_users();

    } else if (strcmp(argv[0], "timeout") == 0) {
        handle_timeout(argc, argv);

    } else if (strcmp(argv[0], "emergency") == 0) {
        handle_emergency(argc, argv);

    } else if (strcmp(argv[0], "evacuation") == 0) {
        handle_evacuation();

    } else if (strcmp(argv[0], "settings") == 0) {
        handle_settings();

    } else if (strcmp(argv[0], "dashboard") == 0) {
        dashboard_show(emergency_active);

    } else if (strcmp(argv[0], "reboot") == 0) {

        if (argc != 1) {
            printf("Usage: reboot\n");
        } else {
            printf("Rebooting...\n");
            fflush(stdout);
            esp_restart();
        }
    }

    return true;
}


/* ADMIN SESSION MONITOR */
void admin_monitor_task(void *arg)
{
    (void)arg;

    while (1) {

        if (admin_logged_in &&
            elapsed_seconds(last_activity) >= timeout_seconds) {

            admin_logged_in = false;

            printf("Session expired due to inactivity.\n");
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}