#pragma once

#include <stdbool.h>

typedef struct {
    bool occupied;
    bool light_on;
    bool motion_detected;
} room_status_t;

void lighting_init(void);

bool rooms_get_status(int room, room_status_t *out);