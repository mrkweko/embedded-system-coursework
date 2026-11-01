#include "energy.h"
#include "lighting.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* Assumed power rating of each room light */
#define ROOM_LIGHT_WATTS 40.0f

/* Accumulated lighting time for both rooms */
static uint32_t light_seconds[2] = {0, 0};

static portMUX_TYPE energy_mux = portMUX_INITIALIZER_UNLOCKED;


/* Monitor lighting usage every second */
static void energy_monitor_task(void *pvParameters)
{
    (void)pvParameters;

    room_status_t room;

    while (1)
    {
        for (int i = 1; i <= 2; i++)
        {
            if (rooms_get_status(i, &room) && room.light_on)
            {
                portENTER_CRITICAL(&energy_mux);

                light_seconds[i - 1]++;

                portEXIT_CRITICAL(&energy_mux);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}


/* Start the energy monitoring task */
void energy_init(void)
{
    xTaskCreate(
        energy_monitor_task,
        "energy_monitor",
        2048,
        NULL,
        4,
        NULL
    );
}


/* Get accumulated lighting time */
uint32_t energy_get_light_seconds(int room)
{
    if (room < 1 || room > 2)
    {
        return 0;
    }

    portENTER_CRITICAL(&energy_mux);

    uint32_t seconds = light_seconds[room - 1];

    portEXIT_CRITICAL(&energy_mux);

    return seconds;
}


/* Calculate estimated energy consumption in kWh */
float energy_get_kwh(int room)
{
    if (room < 1 || room > 2)
    {
        return 0.0f;
    }

    uint32_t seconds = energy_get_light_seconds(room);

    float energy = (ROOM_LIGHT_WATTS * seconds) / 3600000.0f;

    return energy;
}