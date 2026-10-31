#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lighting.h"

static const char *TAG = "LightingSystem";

typedef struct {
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

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static const room_config_t room_1 = {
    .room_name = "Lecture Room 1",
    .pir_pin = GPIO_NUM_13,
    .light_pin = GPIO_NUM_22,
    .off_delay_ticks = pdMS_TO_TICKS(30000)
};

static const room_config_t room_2 = {
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
