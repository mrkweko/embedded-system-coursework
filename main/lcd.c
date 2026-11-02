// lcd.c - 16x2 LCD with an I2C backpack (PCF8574, address 0x27). Added by Member 2.
// Other tasks call lcd_show(); only the LCD task talks to the display, so no lock is needed.
// When nothing is being shown, the screen shows the building state and how many are inside.
#include "lcd.h"
#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "access.h"

#define PIN_SDA   33            // must match diagram.json (21 and 22 are already used)
#define PIN_SCL   4
#define LCD_ADDR  0x27

// PCF8574 backpack: bit0 = RS, bit1 = RW, bit2 = E, bit3 = backlight, bits4-7 = D4-D7
#define BIT_RS 0x01
#define BIT_E  0x04
#define BIT_BL 0x08

typedef struct {
    char line1[17];
    char line2[17];
    int  hold_ms;
} lcd_msg_t;

static i2c_master_dev_handle_t s_dev;
static QueueHandle_t s_queue;

// Send one 4-bit value: raise E, then lower it (the display reads on the falling edge)
static void send_nibble(uint8_t nibble, uint8_t rs)
{
    uint8_t b = (uint8_t)((nibble << 4) | BIT_BL | rs);
    uint8_t out[2] = { (uint8_t)(b | BIT_E), b };
    i2c_master_transmit(s_dev, out, 2, 50);
    esp_rom_delay_us(60);
}

static void send_byte(uint8_t v, uint8_t rs)
{
    send_nibble(v >> 4, rs);
    send_nibble(v & 0x0F, rs);
}

static void lcd_hw_init(void)
{
    vTaskDelay(pdMS_TO_TICKS(50));
    send_nibble(0x3, 0); esp_rom_delay_us(5000);        // wake-up sequence from the datasheet
    send_nibble(0x3, 0); esp_rom_delay_us(1000);
    send_nibble(0x3, 0); esp_rom_delay_us(1000);
    send_nibble(0x2, 0);                                // switch to 4-bit mode
    send_byte(0x28, 0);                                 // 2 lines, 5x8 font
    send_byte(0x0C, 0);                                 // display on, no cursor
    send_byte(0x06, 0);                                 // cursor moves right
    send_byte(0x01, 0); esp_rom_delay_us(3000);         // clear
}

// Write one line, padded with spaces to 16 characters (so no clear and no flicker)
static void write_line(int row, const char *text)
{
    send_byte(row == 0 ? 0x80 : 0xC0, 0);               // move to start of the row
    bool ended = false;
    for (int i = 0; i < 16; i++) {
        ended = ended || text[i] == '\0';
        send_byte(ended ? ' ' : (uint8_t)text[i], BIT_RS);
    }
}

static void draw(const char *l1, const char *l2)
{
    write_line(0, l1);
    write_line(1, l2);
}

// Default screen: building state and number inside (redrawn only when it changes)
static void draw_idle(char last[2][17])
{
    char l1[24], l2[24];
    int st = access_schedule_state();
    snprintf(l1, sizeof(l1), "%s", st == ACCESS_OPEN ? "CoCIS: OPEN" :
                                   st == ACCESS_CLOSED ? "CoCIS: CLOSED" : "EMERGENCY MODE");
    snprintf(l2, sizeof(l2), "Inside: %d", access_inside_count());
    if (strcmp(l1, last[0]) != 0 || strcmp(l2, last[1]) != 0) {
        draw(l1, l2);
        strcpy(last[0], l1);
        strcpy(last[1], l2);
    }
}

static void lcd_task(void *arg)
{
    (void)arg;
    char last[2][17] = { "", "" };
    lcd_msg_t m;
    for (;;) {
        if (xQueueReceive(s_queue, &m, pdMS_TO_TICKS(1000)) == pdTRUE) {
            draw(m.line1, m.line2);
            vTaskDelay(pdMS_TO_TICKS(m.hold_ms));
            last[0][0] = last[1][0] = '\0';             // force the default screen to redraw
        } else {
            draw_idle(last);
        }
    }
}

void lcd_show(const char *line1, const char *line2, int hold_ms)
{
    if (s_queue == NULL) {
        return;
    }
    lcd_msg_t m;
    snprintf(m.line1, sizeof(m.line1), "%s", line1);
    snprintf(m.line2, sizeof(m.line2), "%s", line2);
    m.hold_ms = hold_ms;
    xQueueSend(s_queue, &m, 0);                         // if the queue is full, drop the message
}

void lcd_start(void)
{
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = PIN_SDA,
        .scl_io_num = PIN_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE("lcd", "I2C bus init failed - LCD disabled");
        return;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = LCD_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE("lcd", "cannot add the LCD - LCD disabled");
        return;
    }
    s_queue = xQueueCreate(8, sizeof(lcd_msg_t));
    lcd_hw_init();
    draw("CoCIS Smart Ctrl", "Starting...");
    xTaskCreate(lcd_task, "lcd", 4096, NULL, 3, NULL);
}
