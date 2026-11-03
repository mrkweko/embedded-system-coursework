#pragma once

#include <stdbool.h>

void alarm_start(void);
void alarm_door_open(bool entry);
void alarm_door_close(bool entry);
void alarm_note_denied(void);
