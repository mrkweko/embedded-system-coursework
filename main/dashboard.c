#include <stdio.h>

#include "dashboard.h"
#include "lighting.h"
#include "energy.h"
#include "access.h"

void dashboard_show(bool emergency_active)
{
    room_status_t room;

    printf("\n");
    printf("========================================\n");
    printf("       CoCIS SMART BUILDING\n");
    printf("       MANAGEMENT DASHBOARD\n");
    printf("========================================\n");

    /* BUILDING STATUS */
    printf("\nBUILDING STATUS\n");
    printf("----------------------------------------\n");

    printf("Emergency Status : %s\n",
           emergency_active ? "ACTIVE" : "NORMAL");

    /* ACCESS SCHEDULE */
    int opening_time;
    int closing_time;

    access_get_hours(&opening_time, &closing_time);

    printf("Access Schedule  : %02d:%02d - %02d:%02d\n",
           opening_time / 60, opening_time % 60,
           closing_time / 60, closing_time % 60);

    /* BUILDING ACCESS */
    int building_state = access_schedule_state();

    printf("Building Access  : ");

    if (building_state == ACCESS_OPEN)
    {
        printf("OPEN\n");
    }
    else if (building_state == ACCESS_CLOSED)
    {
        printf("CLOSED\n");
    }
    else if (building_state == ACCESS_EMERGENCY)
    {
        printf("EMERGENCY ACCESS\n");
    }

    /* ROOM MONITORING */
    printf("\nROOM MONITORING\n");
    printf("----------------------------------------\n");

    for (int i = 1; i <= 2; i++)
    {
        if (rooms_get_status(i, &room))
        {
            printf("\nLecture Room %d\n", i);

            printf("Occupancy       : %s\n",
                   room.occupied ? "Occupied" : "Unoccupied");

            printf("Motion          : %s\n",
                   room.motion_detected ? "Detected" : "None");

            printf("Lighting        : %s\n",
                   room.light_on ? "ON" : "OFF");

            printf("Light Usage     : %lu seconds\n",
                   (unsigned long)energy_get_light_seconds(i));

            printf("Energy Used     : %.6f kWh\n",
                   (double)energy_get_kwh(i));
        }
    }

    /* BUILDING ENERGY SUMMARY */
    float total_energy = energy_get_kwh(1) + energy_get_kwh(2);

    printf("\nBUILDING ENERGY SUMMARY\n");
    printf("----------------------------------------\n");

    printf("Total Energy Used : %.6f kWh\n",
           (double)total_energy);

    printf("Light Power Rating: 40 W per room (assumed)\n");

    printf("\n========================================\n");
    printf("          END OF DASHBOARD\n");
    printf("========================================\n");
}