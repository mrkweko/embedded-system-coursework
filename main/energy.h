#pragma once

#include <stdint.h>
#include "esp_err.h"

void energy_init(void);

/* Returns accumulated lighting time in seconds. */
uint32_t energy_get_light_seconds(int room);

/* Returns estimated energy consumption in kWh. */
float energy_get_kwh(int room);