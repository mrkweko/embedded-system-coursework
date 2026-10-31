#include "storage.h"

#include <stdarg.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_err.h"
#include "esp_vfs_fat.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h" // [kweko] build fix: exported by esp_driver_sdmmc
#include "driver/sdspi_host.h"

#define SD_MOUNT_POINT "/sd"
#define SD_CS_PIN GPIO_NUM_4
#define SD_RETRY_TICKS pdMS_TO_TICKS(10000)
#define LOG_FILE_NAME SD_MOUNT_POINT "/events.csv"
#define VALID_TIME_THRESHOLD ((time_t)1700000000)
#define SPI_SCK_PIN 18
#define SPI_MISO_PIN 19
#define SPI_MOSI_PIN 23

static QueueHandle_t s_log_queue;
static SemaphoreHandle_t s_ring_mutex;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;
static log_entry_t s_ring[LOG_RING_SIZE];
static int s_ring_count;
static int s_ring_next;
static uint32_t s_dropped_events;
static uint32_t s_entries_logged;
static bool s_sd_ok;

static const char *event_type_name(log_event_type_t type)
{
    switch (type) {
    case LOG_EVENT_ACCESS_GRANTED:
        return "ACCESS_GRANTED";
    case LOG_EVENT_ACCESS_DENIED:
        return "ACCESS_DENIED";
    case LOG_EVENT_RFID_SCAN:
        return "RFID_SCAN";
    case LOG_EVENT_SYSTEM:
        return "SYSTEM";
    default:
        return "UNKNOWN";
    }
}

static void set_sd_ok(bool value)
{
    portENTER_CRITICAL(&s_status_lock);
    s_sd_ok = value;
    portEXIT_CRITICAL(&s_status_lock);
}

static bool mount_sd(sdmmc_card_t **card)
{
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = false,
        .max_files = 1,
        .allocation_unit_size = 16 * 1024,
    };

    slot.host_id = host.slot;
    slot.gpio_cs = SD_CS_PIN;
    esp_err_t err = esp_vfs_fat_sdspi_mount(
        SD_MOUNT_POINT, &host, &slot, &mount_config, card);
    if (err != ESP_OK) {
        set_sd_ok(false);
        return false;
    }

    FILE *file = fopen(LOG_FILE_NAME, "a");
    if (file == NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, *card);
        set_sd_ok(false);
        return false;
    }
    fclose(file);
    set_sd_ok(true);
    return true;
}

static void unmount_sd(sdmmc_card_t **card)
{
    if (*card != NULL) {
        esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, *card);
        *card = NULL;
    }
    set_sd_ok(false);
}

static void add_to_ring(const log_entry_t *entry)
{
    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    s_ring[s_ring_next] = *entry;
    s_ring_next = (s_ring_next + 1) % LOG_RING_SIZE;
    if (s_ring_count < LOG_RING_SIZE) {
        s_ring_count++;
    }
    xSemaphoreGive(s_ring_mutex);
}

static void format_timestamp(time_t timestamp, TickType_t uptime_ticks,
                             char output[20])
{
    struct tm tm_value;
    if (timestamp >= VALID_TIME_THRESHOLD && localtime_r(&timestamp, &tm_value) != NULL) {
        strftime(output, 20, "%Y-%m-%d %H:%M:%S", &tm_value);
        return;
    }

    /* The simulator's clock may not be set before the first event. */
    uint32_t seconds = (uint32_t)(uptime_ticks * portTICK_PERIOD_MS / 1000);
    snprintf(output, 20, "1970-01-01 %02lu:%02lu:%02lu",
             (unsigned long)((seconds / 3600) % 24),
             (unsigned long)((seconds / 60) % 60),
             (unsigned long)(seconds % 60));
}

static bool append_event(const log_entry_t *entry)
{
    char timestamp[20];
    char message[LOG_MESSAGE_SIZE];
    format_timestamp(entry->timestamp, entry->uptime_ticks, timestamp);
    for (size_t i = 0; i < sizeof(message) - 1; i++) {
        char c = entry->message[i];
        if (c == '\0') {
            message[i] = '\0';
            break;
        }
        message[i] = (c == ',' || c == '\r' || c == '\n') ? ' ' : c;
        message[i + 1] = '\0';
    }

    FILE *file = fopen(LOG_FILE_NAME, "a");
    if (file == NULL) {
        return false;
    }
    int result = fprintf(file, "%s,%s,%s\n", timestamp,
                         event_type_name(entry->type), message);
    bool ok = result >= 0 && fflush(file) == 0;
    int close_result = fclose(file);
    ok = ok && close_result == 0;
    return ok;
}

static void logger_task(void *arg)
{
    (void)arg;
    sdmmc_card_t *card = NULL;
    TickType_t next_retry = 0;
#ifdef CONFIG_LOGGER_STACK_DEBUG
    printf("[LOGGER] stack high-water mark: %u words\n",
           (unsigned)uxTaskGetStackHighWaterMark(NULL));
#endif

    for (;;) {
        log_entry_t entry;
        if (xQueueReceive(s_log_queue, &entry, SD_RETRY_TICKS) != pdTRUE) {
            if (card == NULL && xTaskGetTickCount() >= next_retry) {
                if (mount_sd(&card)) {
                    next_retry = xTaskGetTickCount() + SD_RETRY_TICKS;
                } else {
                    next_retry = xTaskGetTickCount() + SD_RETRY_TICKS;
                }
            }
            continue;
        }

        add_to_ring(&entry);
        portENTER_CRITICAL(&s_status_lock);
        if (s_entries_logged < UINT32_MAX) {
            s_entries_logged++;
        }
        portEXIT_CRITICAL(&s_status_lock);
        if (card == NULL) {
            if (xTaskGetTickCount() >= next_retry) {
                if (mount_sd(&card)) {
                    next_retry = xTaskGetTickCount() + SD_RETRY_TICKS;
                } else {
                    next_retry = xTaskGetTickCount() + SD_RETRY_TICKS;
                }
            }
        }
        if (card != NULL && !append_event(&entry)) {
            unmount_sd(&card);
            next_retry = xTaskGetTickCount() + SD_RETRY_TICKS;
        }
    }
}

bool log_event(log_event_type_t type, const char *fmt, ...)
{
    log_entry_t entry = {
        .timestamp = time(NULL),
        .uptime_ticks = xTaskGetTickCount(),
        .type = type,
    };
    if (fmt == NULL) {
        entry.message[0] = '\0';
    } else {
        va_list args;
        va_start(args, fmt);
        vsnprintf(entry.message, sizeof(entry.message), fmt, args);
        va_end(args);
    }

    if (s_log_queue != NULL && xQueueSend(s_log_queue, &entry, 0) == pdTRUE) {
        return true;
    }

    portENTER_CRITICAL(&s_status_lock);
    s_dropped_events++;
    portEXIT_CRITICAL(&s_status_lock);
    return false;
}

int log_get_recent(int n, log_entry_t *out)
{
    if (n <= 0 || out == NULL || s_ring_mutex == NULL) {
        return 0;
    }

    xSemaphoreTake(s_ring_mutex, portMAX_DELAY);
    int count = n;
    if (count > s_ring_count) {
        count = s_ring_count;
    }
    if (count > LOG_RING_SIZE) {
        count = LOG_RING_SIZE;
    }
    int first = (s_ring_next - s_ring_count + LOG_RING_SIZE) % LOG_RING_SIZE;
    first = (first + s_ring_count - count) % LOG_RING_SIZE;
    for (int i = 0; i < count; i++) {
        out[i] = s_ring[(first + i) % LOG_RING_SIZE];
    }
    xSemaphoreGive(s_ring_mutex);
    return count;
}

uint32_t log_dropped_count(void)
{
    uint32_t dropped;
    portENTER_CRITICAL(&s_status_lock);
    dropped = s_dropped_events;
    portEXIT_CRITICAL(&s_status_lock);
    return dropped;
}

bool log_sd_ok(void)
{
    bool ok;
    portENTER_CRITICAL(&s_status_lock);
    ok = s_sd_ok;
    portEXIT_CRITICAL(&s_status_lock);
    return ok;
}

uint32_t log_entries_count(void)
{
    uint32_t count;
    portENTER_CRITICAL(&s_status_lock);
    count = s_entries_logged;
    portEXIT_CRITICAL(&s_status_lock);
    return count;
}

void logger_print_recent(int n)
{
    log_entry_t entries[LOG_RING_SIZE];
    int count = log_get_recent(n, entries);
    char timestamp[20];
    for (int i = 0; i < count; i++) {
        format_timestamp(entries[i].timestamp, entries[i].uptime_ticks, timestamp);
        printf("%s,%s,%s\n", timestamp, event_type_name(entries[i].type),
               entries[i].message);
    }
    if (count == 0) {
        printf("(no events recorded)\n");
    }
}

int logger_dropped_count(void)
{
    uint32_t dropped = log_dropped_count();
    return dropped > (uint32_t)INT32_MAX ? INT32_MAX : (int)dropped;
}

void storage_start(void)
{
    spi_bus_config_t bus = {
        .mosi_io_num = SPI_MOSI_PIN,
        .miso_io_num = SPI_MISO_PIN,
        .sclk_io_num = SPI_SCK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    esp_err_t spi_err = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
    if (spi_err != ESP_OK && spi_err != ESP_ERR_INVALID_STATE) {
        printf("[LOGGER] SPI bus initialization failed: %s\n",
               esp_err_to_name(spi_err));
    }
    s_ring_mutex = xSemaphoreCreateMutex();
    s_log_queue = xQueueCreate(LOG_QUEUE_LENGTH, sizeof(log_entry_t));
    if (s_ring_mutex == NULL || s_log_queue == NULL) {
        set_sd_ok(false);
        return;
    }
    xTaskCreate(logger_task, "logger", 4096, NULL, 2, NULL);
}
