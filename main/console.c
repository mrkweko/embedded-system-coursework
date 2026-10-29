// console.c - SECTION 7 (Member 4): serial menu + information commands.
// Admin commands (login, schedule, users...) belong to Member 5.
// RULE: never printf while holding a mutex. Copy the data, release, then print.

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart.h"
#include "console.h"
#include "admin.h"

#define CONSOLE_LINE_MAX 96
#define ARGV_MAX 6

// ---- Hook for Member 5: returns true if the command was an admin command ----
// extern bool admin_handle_command(int argc, char *argv[]);

static void print_help(void)
{
    printf("\n==== CoCIS Smart Control : COMMANDS ====\n");
    printf(" help                 show this list\n");
    printf(" status               system summary\n");
    printf(" who                  people currently inside\n");
    printf(" rooms                room occupancy and light status\n");
    printf(" log [n]              last n access events (default 10)\n");
    printf(" users                registered RFID users (admin)\n");
    printf(" scan <uid> <in|out>  simulate an RFID scan\n");
    printf(" -- admin (login required) --\n");
    printf(" login <pin> | logout | schedule set <HH:MM> <HH:MM>\n");
    printf(" time set <HH:MM> | user add/del | pin set | timeout | emergency\n");
    printf("=========================================\n");
}

static void cmd_status(void)
{
    // TODO: take mutex, copy: time, schedule open/closed, people inside,
    //       room1/room2 occupied + light, SD ok?, dropped log events. Release, print.
    printf("---- SYSTEM STATUS ----\nTODO\n");
}

static void cmd_who(void)   { /* TODO: copy inside-list under mutex, then print */ printf("TODO\n"); }
static void cmd_rooms(void) { /* TODO: Member 1's room data */                    printf("TODO\n"); }                           

static void cmd_log(int n)
{
    if (n < 1) n = 10;
    // TODO: read last n entries via Member 3's log API
    printf("Last %d events: TODO\n", n);
}

static void cmd_scan(const char *uid, const char *dir)
{
    scan_event_t ev = {0};
    if (strcmp(dir, "in") == 0)       ev.entry = true;
    else if (strcmp(dir, "out") == 0) ev.entry = false;
    else { printf("Usage: scan <uid> <in|out>\n"); return; }

    strncpy(ev.uid, uid, sizeof(ev.uid) - 1);

    // Same queue a real RFID reader would use, so the entrance task cannot tell the difference.
    if (xQueueSend(scan_queue, &ev, 0) == pdTRUE)
        printf("Scan submitted: %s (%s)\n", ev.uid, dir);
    else
        printf("Scan queue full, try again.\n");
}

// Split a line into words (modifies the buffer). Returns the word count.
static int tokenize(char *line, char *argv[], int maxn)
{
    int argc = 0;
    char *p = strtok(line, " \t");
    while (p && argc < maxn) { argv[argc++] = p; p = strtok(NULL, " \t"); }
    return argc;
}

static void handle_line(char *line)
{
    char *argv[ARGV_MAX];
    int argc = tokenize(line, argv, ARGV_MAX);
    if (argc == 0) return;

    for (char *c = argv[0]; *c; c++) *c = (char)tolower((unsigned char)*c);

    if (admin_handle_command(argc, argv)) return;

    if      (!strcmp(argv[0], "help"))   print_help();
    else if (!strcmp(argv[0], "status")) cmd_status();
    else if (!strcmp(argv[0], "who"))    cmd_who();
    else if (!strcmp(argv[0], "rooms"))  cmd_rooms();
    else if (!strcmp(argv[0], "log"))    cmd_log(argc > 1 ? atoi(argv[1]) : 10);
    else if (!strcmp(argv[0], "scan")) {
        if (argc < 3) printf("Usage: scan <uid> <in|out>\n");
        else          cmd_scan(argv[1], argv[2]);
    }
    else printf("Unknown command '%s'. Type 'help'.\n", argv[0]);
}

static void console_task(void *arg)
{
    char buf[CONSOLE_LINE_MAX];
    int len = 0;
    uint8_t c;

    print_help();
    printf("> "); fflush(stdout);

    for (;;) {
        // Waits up to 20 ms for one byte; the task sleeps meanwhile, so others can run.
        if (uart_read_bytes(UART_NUM_0, &c, 1, pdMS_TO_TICKS(20)) == 1) {
            if (c == '\r' || c == '\n') {
                printf("\n");
                if (len > 0) { buf[len] = 0; handle_line(buf); len = 0; }
                printf("> ");
            } else if ((c == 8 || c == 127) && len > 0) {      // backspace
                len--;
                printf("\b \b");
            } else if (len < CONSOLE_LINE_MAX - 1 && c >= 32) {
                buf[len++] = (char)c;
                printf("%c", c);                                // echo
            }
            fflush(stdout);
        }
    }
}

void console_start(void)
{
    uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
    xTaskCreate(console_task, "console", 4096, NULL, 3, NULL);
}
