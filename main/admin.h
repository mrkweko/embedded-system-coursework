
#ifndef ADMIN_H
#define ADMIN_H

#include <stdbool.h>
#include "esp_err.h"

esp_err_t admin_start(void);

bool admin_handle_command(int argc, char *argv[]);

void admin_monitor_task(void *arg);
void admin_state_snapshot(char pin[9], unsigned int *timeout_seconds,
                          int *current_time);

#endif
