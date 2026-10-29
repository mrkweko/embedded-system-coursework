// rfid.c - Member 2: two MFRC522 RFID readers (ENTRY and EXIT) sharing one SPI bus.
// A task polls both readers. When a card is shown, it reads the card ID, turns it into text
// like "01020304" and puts it in scan_queue for access.c.
// Minimal driver: 4-byte card IDs only (Wokwi's Blue, Green, Yellow, Red cards and the Key Fob).
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "console.h"                // scan_event_t, scan_queue

// Pins (must match diagram.json). SCK, MISO and MOSI are shared; each reader has its own SDA and RST.
#define PIN_SCK   18
#define PIN_MISO  19
#define PIN_MOSI  23

// MFRC522 registers
#define CommandReg    0x01
#define ComIrqReg     0x04
#define ErrorReg      0x06
#define FIFODataReg   0x09
#define FIFOLevelReg  0x0A
#define BitFramingReg 0x0D
#define CollReg       0x0E
#define ModeReg       0x11
#define TxControlReg  0x14
#define TxASKReg      0x15
#define TModeReg      0x2A
#define TPrescalerReg 0x2B
#define TReloadRegH   0x2C
#define TReloadRegL   0x2D
#define VersionReg    0x37

typedef struct {
    bool                entry;          // true = entry reader, false = exit reader
    gpio_num_t          cs, rst;        // the reader's SDA (chip select) and reset pins
    spi_device_handle_t dev;
    char                last_uid[16];   // used to ignore the same card read again and again
    int64_t             last_us;
} reader_t;

static reader_t s_readers[2] = {
    { .entry = true,  .cs = GPIO_NUM_5,  .rst = GPIO_NUM_21 },
    { .entry = false, .cs = GPIO_NUM_17, .rst = GPIO_NUM_16 },
};

// Read or write one register of the chip over SPI
static uint8_t reg_read(reader_t *r, uint8_t reg)
{
    uint8_t tx[2] = { (uint8_t)(0x80 | (reg << 1)), 0 }, rx[2] = { 0, 0 };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx, .rx_buffer = rx };
    spi_device_polling_transmit(r->dev, &t);
    return rx[1];
}

static void reg_write(reader_t *r, uint8_t reg, uint8_t val)
{
    uint8_t tx[2] = { (uint8_t)(reg << 1), val };
    spi_transaction_t t = { .length = 16, .tx_buffer = tx };
    spi_device_polling_transmit(r->dev, &t);
}

// Send bytes to the card and wait for its answer. 'bits' = bits of the last byte to send (7 for the first request).
static bool transceive(reader_t *r, const uint8_t *send, int len, uint8_t *back, int *back_len, uint8_t bits)
{
    reg_write(r, CommandReg, 0x00);                     // idle
    reg_write(r, ComIrqReg, 0x7F);                      // clear flags
    reg_write(r, FIFOLevelReg, 0x80);                   // empty the FIFO
    for (int i = 0; i < len; i++) {
        reg_write(r, FIFODataReg, send[i]);
    }
    reg_write(r, BitFramingReg, bits);
    reg_write(r, CommandReg, 0x0C);                     // transceive
    reg_write(r, BitFramingReg, bits | 0x80);           // start sending

    int64_t deadline = esp_timer_get_time() + 36000;    // wait up to 36 ms
    uint8_t irq = 0;
    while (!(irq & 0x31) && esp_timer_get_time() < deadline) {
        irq = reg_read(r, ComIrqReg);
    }
    if (!(irq & 0x30) || (reg_read(r, ErrorReg) & 0x1B)) {
        return false;                                   // no answer, or an error
    }
    int n = reg_read(r, FIFOLevelReg);
    if (n > *back_len) {
        n = *back_len;
    }
    if (n > 16) {
        n = 16;
    }
    if (n > 0) {
        // Burst read: send the FIFO address n times, then 0. Data arrives in rx[1..n].
        uint8_t tx[17], rx[17] = { 0 };
        for (int i = 0; i < n; i++) {
            tx[i] = (uint8_t)(0x80 | (FIFODataReg << 1));
        }
        tx[n] = 0;
        spi_transaction_t t = { .length = (n + 1) * 8, .tx_buffer = tx, .rx_buffer = rx };
        spi_device_polling_transmit(r->dev, &t);
        for (int i = 0; i < n; i++) {
            back[i] = rx[i + 1];
        }
    }
    *back_len = n;
    return true;
}

// Is a card in front of the reader? If so, get its 4-byte ID into uid[0..3] (uid[4] is a check byte).
static bool read_card(reader_t *r, uint8_t uid[5])
{
    uint8_t reqa = 0x26, atqa[2], ask_id[2] = { 0x93, 0x20 };
    int n = 2;
    reg_write(r, CollReg, reg_read(r, CollReg) & 0x7F);
    if (!transceive(r, &reqa, 1, atqa, &n, 0x07)) {     // 1. "Is anyone there?"
        return false;
    }
    n = 5;
    if (!transceive(r, ask_id, 2, uid, &n, 0) || n != 5) {   // 2. "Tell me your ID"
        return false;
    }
    return (uid[0] ^ uid[1] ^ uid[2] ^ uid[3]) == uid[4];    // 3. check byte must match
}

static void reader_init(reader_t *r)
{
    gpio_config_t io = { .pin_bit_mask = 1ULL << r->rst, .mode = GPIO_MODE_OUTPUT };
    gpio_config(&io);
    spi_device_interface_config_t dev = { .clock_speed_hz = 1000000, .mode = 0, .spics_io_num = r->cs, .queue_size = 1 };
    spi_bus_add_device(SPI2_HOST, &dev, &r->dev);

    gpio_set_level(r->rst, 0);                          // reset the chip
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(r->rst, 1);
    vTaskDelay(pdMS_TO_TICKS(50));

    // Timer and signal settings, copied from the Arduino MFRC522 library
    static const uint8_t settings[][2] = {
        { TModeReg, 0x80 }, { TPrescalerReg, 0xA9 }, { TReloadRegH, 0x03 },
        { TReloadRegL, 0xE8 }, { TxASKReg, 0x40 }, { ModeReg, 0x3D },
    };
    for (int i = 0; i < 6; i++) {
        reg_write(r, settings[i][0], settings[i][1]);
    }
    reg_write(r, TxControlReg, reg_read(r, TxControlReg) | 0x03);   // antenna on

    uint8_t ver = reg_read(r, VersionReg);              // 0x91 or 0x92 = chip answered; 0x00 or 0xFF = bad wiring
    ESP_LOGI("rfid", "%s reader: chip version 0x%02X%s", r->entry ? "ENTRY" : "EXIT", ver,
             (ver == 0x00 || ver == 0xFF) ? "  <-- no answer, check the wiring" : "");
}

static void rfid_task(void *arg)
{
    (void)arg;
    for (;;) {
        for (int i = 0; i < 2; i++) {
            reader_t *r = &s_readers[i];
            uint8_t uid[5];
            if (!read_card(r, uid)) {
                continue;
            }
            scan_event_t ev = { .entry = r->entry };
            snprintf(ev.uid, sizeof(ev.uid), "%02X%02X%02X%02X", uid[0], uid[1], uid[2], uid[3]);

            // Same card still in front of the reader (seen within the last 2 s)? Count it once.
            int64_t now = esp_timer_get_time();
            bool same = strcmp(ev.uid, r->last_uid) == 0 && now - r->last_us < 2000000;
            strlcpy(r->last_uid, ev.uid, sizeof(r->last_uid));
            r->last_us = now;
            if (same) {
                continue;
            }
            printf("\n[RFID] %s reader: card %s\n", r->entry ? "ENTRY" : "EXIT", ev.uid);
            xQueueSend(scan_queue, &ev, 0);
        }
        vTaskDelay(pdMS_TO_TICKS(20));                  // poll often so a short TAP is not missed
    }
}

void rfid_start(void)
{
    spi_bus_config_t bus = { .mosi_io_num = PIN_MOSI, .miso_io_num = PIN_MISO, .sclk_io_num = PIN_SCK,
                             .quadwp_io_num = -1, .quadhd_io_num = -1, .max_transfer_sz = 4000 };
    spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);   // harmless if the SD card code started the bus first
    reader_init(&s_readers[0]);
    reader_init(&s_readers[1]);
    xTaskCreate(rfid_task, "rfid", 4096, NULL, 4, NULL);
}