#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lighting.h"

static const char *TAG = "LightingSystem";

static portMUX_TYPE room_status_mux = portMUX_INITIALIZER_UNLOCKED;
static room_status_t room_status[2];

typedef struct {
    int room_number;
    const char *room_name;
    gpio_num_t pir_pin;
    gpio_num_t light_pin;
    TickType_t off_delay_ticks;
} room_config_t;

static void room_control_task(void *pvParameters)
{
    const room_config_t *config = (const room_config_t *)pvParameters;

    gpio_reset_pin(config->pir_pin);
    gpio_set_direction(config->pir_pin, GPIO_MODE_INPUT);
    gpio_pullup_dis(config->pir_pin);

    gpio_reset_pin(config->light_pin);
    gpio_set_direction(config->light_pin, GPIO_MODE_OUTPUT);
    gpio_set_level(config->light_pin, 0);

    TickType_t last_motion_time = 0;
    bool light_is_on = false;

    ESP_LOGI(TAG, "Task started for: %s", config->room_name);

    while (1) {
        int motion = gpio_get_level(config->pir_pin);
        TickType_t now = xTaskGetTickCount();

        if (motion) {
            last_motion_time = now;
            if (!light_is_on) {
                gpio_set_level(config->light_pin, 1);
                light_is_on = true;
                ESP_LOGI(TAG, "[%s] Motion detected, light ON", config->room_name);
            }
        } else if (light_is_on &&
                   (now - last_motion_time) >= config->off_delay_ticks) {
            gpio_set_level(config->light_pin, 0);
            light_is_on = false;
            ESP_LOGI(TAG, "[%s] Timeout, light OFF", config->room_name);
        }

        portENTER_CRITICAL(&room_status_mux);

        room_status[config->room_number - 1].occupied =
            light_is_on && (now - last_motion_time) < config->off_delay_ticks;

        room_status[config->room_number - 1].light_on = light_is_on;

        room_status[config->room_number - 1].motion_detected = (motion != 0);

        portEXIT_CRITICAL(&room_status_mux);
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static const room_config_t room_1 = {
    .room_number = 1,
    .room_name = "Lecture Room 1",
    .pir_pin = GPIO_NUM_13,
    .light_pin = GPIO_NUM_22,
    .off_delay_ticks = pdMS_TO_TICKS(30000)
};

static const room_config_t room_2 = {
    .room_number = 2,
    .room_name = "Lecture Room 2",
    .pir_pin = GPIO_NUM_14,
    .light_pin = GPIO_NUM_27,
    .off_delay_ticks = pdMS_TO_TICKS(30000)
};

void lighting_init(void)
{
    if (xTaskCreate(room_control_task, "room_1_task", 4096,
                    (void *)&room_1, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lecture Room 1 task");
    }

    if (xTaskCreate(room_control_task, "room_2_task", 4096,
                    (void *)&room_2, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create Lecture Room 2 task");
    }
}

bool rooms_get_status(int room, room_status_t *out)
{
    if (out == NULL || room < 1 || room > 2) {
        return false;
    }

    portENTER_CRITICAL(&room_status_mux);
    *out = room_status[room - 1];
    portEXIT_CRITICAL(&room_status_mux);
    return true;
}
