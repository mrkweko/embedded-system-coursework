
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "admin.h"

/* System settings */
#define DEFAULT_ADMIN_PIN "1234"
#define MAX_LOGIN_ATTEMPTS 3
#define LOCKOUT_SECONDS 60
#define MAX_USERS 20
#define MAX_UID_LENGTH 15
#define DEFAULT_TIMEOUT 120

/* Information about a registered user */
typedef struct {
    char uid[MAX_UID_LENGTH + 1];
    bool active;
} admin_user_t;

/* Administrator information */
static char admin_pin[9] = DEFAULT_ADMIN_PIN;
static bool admin_logged_in = false;
static int failed_attempts = 0;
static bool account_locked = false;

static TickType_t lockout_start = 0;
static TickType_t last_activity = 0;

static unsigned int timeout_seconds = DEFAULT_TIMEOUT;

/* Access schedule, stored as minutes after midnight */
static int opening_time = 7 * 60;
static int closing_time = 20 * 60;
static int current_time = 8 * 60;

/* User records */
static admin_user_t users[MAX_USERS];

/* Emergency mode */
static bool emergency_active = false;


/* Convert a time such as 07:30 into minutes */
static bool convert_time(char *text, int *minutes)
{
    int hour;
    int minute;
    char extra;

    if (sscanf(text, "%d:%d%c", &hour, &minute, &extra) != 2) {
        return false;
    }

    if (hour < 0 || hour > 23 ||
        minute < 0 || minute > 59) {
        return false;
    }

    *minutes = hour * 60 + minute;
    return true;
}


/* Calculate elapsed seconds */
static unsigned int elapsed_seconds(TickType_t start)
{
    return (unsigned int)(
        (xTaskGetTickCount() - start) / configTICK_RATE_HZ
    );
}


/* Check whether the administrator account is locked */
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


/* Find a registered user */
static int find_user(const char *uid)
{
    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active &&
            strcmp(users[i].uid, uid) == 0) {
            return i;
        }
    }

    return -1;
}


/* Administrator login */
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

        printf("Incorrect PIN. Attempts remaining: %d\n",
               MAX_LOGIN_ATTEMPTS - failed_attempts);

        if (failed_attempts >= MAX_LOGIN_ATTEMPTS) {
            account_locked = true;
            admin_logged_in = false;
            lockout_start = xTaskGetTickCount();

            printf("Account locked for %d seconds.\n",
                   LOCKOUT_SECONDS);
        }
    }
}


/* Administrator logout */
static void handle_logout(void)
{
    if (admin_logged_in) {
        admin_logged_in = false;
        printf("Administrator logged out.\n");
    } else {
        printf("You are not logged in.\n");
    }
}


/* Change the administrator PIN */
static void handle_pin(int argc, char *argv[])
{
    if (argc != 3 || strcmp(argv[1], "set") != 0) {
        printf("Usage: pin set <newPIN>\n");
        return;
    }

    int length = strlen(argv[2]);

    if (length < 4 || length > 8) {
        printf("PIN must contain 4 to 8 digits.\n");
        return;
    }

    for (int i = 0; i < length; i++) {
        if (argv[2][i] < '0' || argv[2][i] > '9') {
            printf("PIN must contain digits only.\n");
            return;
        }
    }

    strcpy(admin_pin, argv[2]);
    printf("Administrator PIN changed successfully.\n");
}


/* Change the access schedule */
static void handle_schedule(int argc, char *argv[])
{
    int open;
    int close;

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

    printf("Access schedule updated: %s to %s\n",
           argv[2], argv[3]);

    printf("Member 2's access task must use this schedule.\n");
}


/* Set the simulated current time */
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

    printf("Simulated time updated to %s.\n", argv[2]);
}


/* Add or delete a registered user */
static void handle_user(int argc, char *argv[])
{
    if (argc != 3) {
        printf("Usage: user add <UID>\n");
        printf("   or: user del <UID>\n");
        return;
    }

    if (strlen(argv[2]) == 0 ||
        strlen(argv[2]) > MAX_UID_LENGTH) {
        printf("UID must contain 1 to %d characters.\n",
               MAX_UID_LENGTH);
        return;
    }

    if (strcmp(argv[1], "add") == 0) {
        if (find_user(argv[2]) >= 0) {
            printf("User already exists.\n");
            return;
        }

        for (int i = 0; i < MAX_USERS; i++) {
            if (!users[i].active) {
                strcpy(users[i].uid, argv[2]);
                users[i].active = true;

                printf("User %s registered.\n", argv[2]);
                return;
            }
        }

        printf("User list is full.\n");
    } else if (strcmp(argv[1], "del") == 0) {
        int index = find_user(argv[2]);

        if (index < 0) {
            printf("User not found.\n");
            return;
        }

        users[index].active = false;
        users[index].uid[0] = '\0';

        printf("User %s deleted.\n", argv[2]);
    } else {
        printf("Use 'add' or 'del'.\n");
    }
}


/* Display registered users */
static void handle_users(void)
{
    int count = 0;

    printf("\n--- REGISTERED USERS ---\n");

    for (int i = 0; i < MAX_USERS; i++) {
        if (users[i].active) {
            printf("%d. %s\n", ++count, users[i].uid);
        }
    }

    if (count == 0) {
        printf("No registered users.\n");
    }
}


/* Set automatic logout time */
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

    printf("Automatic logout set to %u seconds.\n",
           timeout_seconds);
}


/* Activate or deactivate emergency mode */
static void handle_emergency(int argc, char *argv[])
{
    if (argc != 2) {
        printf("Usage: emergency <on|off>\n");
        return;
    }

    if (strcmp(argv[1], "on") == 0) {
        emergency_active = true;
        printf("EMERGENCY MODE ACTIVATED.\n");
        printf("Physical door control requires system integration.\n");
    } else if (strcmp(argv[1], "off") == 0) {
        emergency_active = false;
        printf("Emergency mode deactivated.\n");
    } else {
        printf("Use emergency on or emergency off.\n");
    }
}


/* Display the current settings */
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


/* Start the administration system */
esp_err_t admin_start(void)
{
    admin_logged_in = false;
    failed_attempts = 0;
    account_locked = false;
    emergency_active = false;

    printf("\n=== CoCIS ADMIN INTERFACE ===\n");
    printf("Use login 1234 to begin.\n");

    return ESP_OK;
}


/* Process commands from the Serial Monitor */
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

    /* Recognize administrative commands */
    bool admin_command =
        strcmp(argv[0], "pin") == 0 ||
        strcmp(argv[0], "schedule") == 0 ||
        strcmp(argv[0], "time") == 0 ||
        strcmp(argv[0], "user") == 0 ||
        strcmp(argv[0], "users") == 0 ||
        strcmp(argv[0], "timeout") == 0 ||
        strcmp(argv[0], "emergency") == 0 ||
        strcmp(argv[0], "settings") == 0;

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
    } else if (strcmp(argv[0], "settings") == 0) {
        handle_settings();
    }

    return true;
}


/* Check administrator inactivity every second */
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

