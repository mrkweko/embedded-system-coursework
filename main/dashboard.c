
#include <stdio.h>

#include "dashboard.h"
#include "lighting.h"
#include "energy.h"
#include "access.h"

#define MAX_DISPLAY_EVENTS 5

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

    int building_state = access_schedule_state();

    printf("Building Access  : ");

    if (building_state == ACCESS_OPEN)
        printf("OPEN\n");
    else if (building_state == ACCESS_CLOSED)
        printf("CLOSED\n");
    else if (building_state == ACCESS_EMERGENCY)
        printf("EMERGENCY ACCESS - EXIT ONLY\n");

    /* BUILDING SECURITY SUMMARY */
    printf("\nBUILDING SECURITY SUMMARY\n");
    printf("----------------------------------------\n");

    printf("People Inside       : %d\n",
           access_inside_count());

    printf("Security Alerts     : %d\n",
           access_security_alerts());

    printf("Denied Scans        : %d\n",
           access_denied_count());

    printf("Access Inconsistencies: %d\n",
           access_inconsistencies());

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
    float total_energy =
        energy_get_kwh(1) + energy_get_kwh(2);

    printf("\nBUILDING ENERGY SUMMARY\n");
    printf("----------------------------------------\n");

    printf("Total Energy Used : %.6f kWh\n",
           (double)total_energy);

    printf("Light Power Rating: 40 W per room (assumed)\n");

    /* RECENT SECURITY EVENTS */
    printf("\nRECENT SECURITY EVENTS\n");
    printf("----------------------------------------\n");

    security_event_t events[MAX_DISPLAY_EVENTS];

    int event_count =
        access_get_security_events(events, MAX_DISPLAY_EVENTS);

    if (event_count == 0)
    {
        printf("No security events recorded.\n");
    }
    else
    {
        for (int i = 0; i < event_count; i++)
        {
            printf("%d. %-15s | UID: %-8s | %s\n",
                   i + 1,
                   events[i].kind,
                   events[i].uid,
                   events[i].entry ? "ENTRY" : "EXIT");
        }
    }

    printf("\n========================================\n");
    printf("          END OF DASHBOARD\n");
    printf("========================================\n");
}

