#include "alarm.h"

#include "access.h"
#include "admin.h"
#include "lcd.h"
#include "storage.h"

#include "driver/gpio.h"
#include "driver/ledc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define SERVO_ENTRY_PIN GPIO_NUM_15
#define SERVO_EXIT_PIN  GPIO_NUM_2
#define BUZZER_PIN      GPIO_NUM_12
#define BUTTON_PIN      GPIO_NUM_34

#define ALARM_PERIOD_MS       20
#define DENIED_WINDOW_MS      60000
#define SECURITY_ALERT_MS     5000
#define SECURITY_BEEP_MS      150
#define SIREN_TONE_MS         300

static portMUX_TYPE s_alarm_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_entry_open;
static bool s_exit_open;
static bool s_denied_pending;
static int s_denied_count;
static TickType_t s_first_denied_tick;

static uint32_t servo_duty(int angle)
{
    uint32_t pulse_us = 500U + (uint32_t)angle * 1900U / 180U;
    return pulse_us * 16384U / 20000U;
}

static void set_servo(bool entry, int angle)
{
    ledc_set_duty(LEDC_LOW_SPEED_MODE,
                  entry ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1,
                  servo_duty(angle));
    ledc_update_duty(LEDC_LOW_SPEED_MODE,
                     entry ? LEDC_CHANNEL_0 : LEDC_CHANNEL_1);
}

static void buzzer_set(bool on, uint32_t frequency)
{
    if (on) {
        ledc_set_freq(LEDC_LOW_SPEED_MODE, LEDC_TIMER_1, frequency);
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 512);
    } else {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2, 0);
    }
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_2);
}

static void alarm_task(void *arg)
{
    (void)arg;

    bool emergency_was_on = false;
    bool button_armed = true;
    int low_samples = 0;
    bool security_on = false;
    TickType_t security_start = 0;
    TickType_t security_phase = 0;
    bool siren_high = false;
    TickType_t siren_phase = 0;

    for (;;) {
        int button_level = gpio_get_level(BUTTON_PIN);
        if (button_level == 0) {
            if (button_armed && ++low_samples >= 3) {
                admin_set_emergency(access_schedule_state() != ACCESS_EMERGENCY);
                button_armed = false;
                low_samples = 0;
            }
        } else {
            low_samples = 0;
            button_armed = true;
        }

        bool emergency = access_schedule_state() == ACCESS_EMERGENCY;
        TickType_t now = xTaskGetTickCount();

        if (emergency != emergency_was_on) {
            emergency_was_on = emergency;
            security_on = false;
            portENTER_CRITICAL(&s_alarm_mux);
            s_denied_pending = false;
            portEXIT_CRITICAL(&s_alarm_mux);

            if (emergency) {
                set_servo(true, 90);
                set_servo(false, 90);
                siren_high = false;
                siren_phase = now;
                buzzer_set(true, 1000);
                log_event(LOG_EVENT_SYSTEM, "alarm: doors open, siren on");
            } else {
                portENTER_CRITICAL(&s_alarm_mux);
                s_entry_open = false;
                s_exit_open = false;
                portEXIT_CRITICAL(&s_alarm_mux);
                set_servo(true, 0);
                set_servo(false, 0);
                buzzer_set(false, 2000);
                log_event(LOG_EVENT_SYSTEM, "alarm: doors closed, siren off");
            }
        }

        if (emergency) {
            if ((now - siren_phase) >= pdMS_TO_TICKS(SIREN_TONE_MS)) {
                siren_phase = now;
                siren_high = !siren_high;
                buzzer_set(true, siren_high ? 2000 : 1000);
            }
        } else {
            bool denied_pending;
            bool entry_open;
            bool exit_open;

            portENTER_CRITICAL(&s_alarm_mux);
            denied_pending = s_denied_pending;
            s_denied_pending = false;
            entry_open = s_entry_open;
            exit_open = s_exit_open;
            portEXIT_CRITICAL(&s_alarm_mux);

            if (denied_pending) {
                security_on = true;
                security_start = now;
                security_phase = now;
                log_event(LOG_EVENT_SYSTEM, "alarm: repeated denied access");
                lcd_show("SECURITY ALERT", "Repeated denials", 3000);
            }

            if (security_on) {
                if ((now - security_start) >= pdMS_TO_TICKS(SECURITY_ALERT_MS)) {
                    security_on = false;
                    buzzer_set(false, 2000);
                } else {
                    bool beep_on = ((now - security_phase) / pdMS_TO_TICKS(SECURITY_BEEP_MS)) % 2 == 0;
                    buzzer_set(beep_on, 2000);
                }
            } else {
                buzzer_set(false, 2000);
            }

            set_servo(true, entry_open ? 90 : 0);
            set_servo(false, exit_open ? 90 : 0);
        }

        vTaskDelay(pdMS_TO_TICKS(ALARM_PERIOD_MS));
    }
}

void alarm_start(void)
{
    ledc_timer_config_t servo_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_0,
        .duty_resolution = LEDC_TIMER_14_BIT,
        .freq_hz = 50,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&servo_timer);

    ledc_timer_config_t buzzer_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LEDC_TIMER_1,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 2000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ledc_timer_config(&buzzer_timer);

    ledc_channel_config_t entry_channel = {
        .gpio_num = SERVO_ENTRY_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_0,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = servo_duty(0),
        .hpoint = 0,
    };
    ledc_channel_config(&entry_channel);

    ledc_channel_config_t exit_channel = {
        .gpio_num = SERVO_EXIT_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_1,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_0,
        .duty = servo_duty(0),
        .hpoint = 0,
    };
    ledc_channel_config(&exit_channel);

    ledc_channel_config_t buzzer_channel = {
        .gpio_num = BUZZER_PIN,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LEDC_CHANNEL_2,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LEDC_TIMER_1,
        .duty = 0,
        .hpoint = 0,
    };
    ledc_channel_config(&buzzer_channel);

    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << BUTTON_PIN,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&button_config);

    xTaskCreate(alarm_task, "alarm", 4096, NULL, 3, NULL);
}

void alarm_door_open(bool entry)
{
    portENTER_CRITICAL(&s_alarm_mux);
    if (entry) {
        s_entry_open = true;
    } else {
        s_exit_open = true;
    }
    portEXIT_CRITICAL(&s_alarm_mux);
}

void alarm_door_close(bool entry)
{
    if (access_schedule_state() == ACCESS_EMERGENCY) {
        return;
    }

    portENTER_CRITICAL(&s_alarm_mux);
    if (entry) {
        s_entry_open = false;
    } else {
        s_exit_open = false;
    }
    portEXIT_CRITICAL(&s_alarm_mux);
}

void alarm_note_denied(void)
{
    TickType_t now = xTaskGetTickCount();

    portENTER_CRITICAL(&s_alarm_mux);
    if (s_denied_count == 0 ||
        now - s_first_denied_tick > pdMS_TO_TICKS(DENIED_WINDOW_MS)) {
        s_denied_count = 1;
        s_first_denied_tick = now;
    } else {
        s_denied_count++;
    }

    if (s_denied_count >= 3) {
        s_denied_count = 0;
        s_denied_pending = true;
    }
    portEXIT_CRITICAL(&s_alarm_mux);
}
