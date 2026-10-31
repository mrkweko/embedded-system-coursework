#include "state.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "freertos/semphr.h"
#include "shared.h"
#include "storage.h"

#define STATE_NAMESPACE "cocis_state"
#define STATE_VERSION 2U
#define STATE_VALID_SENTINEL 0xC0C15A7EU
#define STATE_DEFAULT_OPEN_MIN (7 * 60)
#define STATE_DEFAULT_CLOSE_MIN (20 * 60)
#define STATE_DEFAULT_TIMEOUT 120U
#define STATE_DEFAULT_CURRENT_TIME (8 * 60)

typedef struct {
    uint32_t version;
    uint32_t sentinel;
    int32_t open_min;
    int32_t close_min;
    int32_t current_time;
    uint32_t timeout_seconds;
    bool emergency;
    char admin_pin[9];
    int32_t n_users;
    time_t last_save_time;
    user_t users[MAX_USERS];
} state_blob_t;

static int s_open_min = STATE_DEFAULT_OPEN_MIN;
static int s_close_min = STATE_DEFAULT_CLOSE_MIN;
static int s_current_time = STATE_DEFAULT_CURRENT_TIME;
static unsigned int s_timeout_seconds = STATE_DEFAULT_TIMEOUT;
static bool s_emergency;
static char s_admin_pin[9] = "1234";
static bool s_loaded;
static bool s_dirty;
static uint32_t s_dirty_generation;
static int s_loaded_records;
static SemaphoreHandle_t s_state_mutex;
static SemaphoreHandle_t s_nvs_mutex;
static time_t s_last_save_time;

static uint32_t state_crc(const state_blob_t *state)
{
    const uint8_t *bytes = (const uint8_t *)state;
    uint32_t crc = 2166136261U;
    for (size_t i = 0; i < sizeof(*state); i++) {
        crc ^= bytes[i];
        crc *= 16777619U;
    }
    return crc;
}

static void set_defaults(void)
{
    s_open_min = STATE_DEFAULT_OPEN_MIN;
    s_close_min = STATE_DEFAULT_CLOSE_MIN;
    s_current_time = STATE_DEFAULT_CURRENT_TIME;
    s_timeout_seconds = STATE_DEFAULT_TIMEOUT;
    s_emergency = false;
    strlcpy(s_admin_pin, "1234", sizeof(s_admin_pin));
    s_loaded = false;
    s_loaded_records = 0;
}

static bool valid_blob(const state_blob_t *state, size_t size, uint32_t crc)
{
    if (size != sizeof(*state) ||
        state->version != STATE_VERSION ||
        state->sentinel != STATE_VALID_SENTINEL ||
        state->n_users < 0 || state->n_users > MAX_USERS ||
        state->open_min < 0 || state->open_min >= 24 * 60 ||
        state->close_min < 0 || state->close_min >= 24 * 60 ||
        state->timeout_seconds < 10 || state->timeout_seconds > 3600 ||
        state->admin_pin[0] == '\0' ||
        state->admin_pin[sizeof(state->admin_pin) - 1] != '\0') {
        return false;
    }
    return state_crc(state) == crc;
}

void state_load(void)
{
    s_state_mutex = xSemaphoreCreateMutex();
    s_nvs_mutex = xSemaphoreCreateMutex();
    set_defaults();
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }

    state_blob_t state = {0};
    uint32_t crc = 0;
    size_t size = sizeof(state);
    bool restored = false;
    nvs_handle_t handle = 0;
    bool read_ok = false;
    if (err == ESP_OK && nvs_open(STATE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        read_ok = nvs_get_blob(handle, "state", &state, &size) == ESP_OK &&
                  nvs_get_u32(handle, "crc", &crc) == ESP_OK;
        nvs_close(handle);
    }
    if (read_ok && valid_blob(&state, size, crc)) {
        s_open_min = state.open_min;
        s_close_min = state.close_min;
        s_current_time = state.current_time;
        s_timeout_seconds = state.timeout_seconds;
        s_emergency = state.emergency;
        s_last_save_time = state.last_save_time;
        memcpy(s_admin_pin, state.admin_pin, sizeof(s_admin_pin));

        xSemaphoreTake(g_users_mutex, portMAX_DELAY);
        g_n_users = state.n_users;
        memcpy(g_users, state.users, sizeof(g_users));
        xSemaphoreGive(g_users_mutex);
        s_loaded = true;
        restored = true;
    } else {
        xSemaphoreTake(g_users_mutex, portMAX_DELAY);
        memset(g_users, 0, sizeof(g_users));
        g_n_users = 0;
        xSemaphoreGive(g_users_mutex);
    }

    s_loaded_records = restored ? state.n_users : 0;
}

void state_log_boot(void)
{
    log_event(LOG_EVENT_SYSTEM, "state %s, %d user records",
              s_loaded ? "restored" : "defaulted", s_loaded_records);
}

bool state_get_loaded_access(int *open_min, int *close_min, bool *emergency)
{
    if (!open_min || !close_min || !emergency) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    *open_min = s_open_min;
    *close_min = s_close_min;
    *emergency = s_emergency;
    bool loaded = s_loaded;
    xSemaphoreGive(s_state_mutex);
    return loaded;
}

bool state_get_loaded_admin(char pin[9], unsigned int *timeout_seconds,
                            int *current_time, bool *emergency)
{
    if (!pin || !timeout_seconds || !current_time || !emergency) {
        return false;
    }
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    strlcpy(pin, s_admin_pin, 9);
    *timeout_seconds = s_timeout_seconds;
    *current_time = s_current_time;
    *emergency = s_emergency;
    bool loaded = s_loaded;
    xSemaphoreGive(s_state_mutex);
    return loaded;
}

void state_update_access(int open_min, int close_min, bool emergency)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_open_min = open_min;
    s_close_min = close_min;
    s_emergency = emergency;
    xSemaphoreGive(s_state_mutex);
}

void state_update_admin(const char pin[9], unsigned int timeout_seconds,
                        int current_time)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (pin != NULL) {
        strlcpy(s_admin_pin, pin, sizeof(s_admin_pin));
    }
    s_timeout_seconds = timeout_seconds;
    s_current_time = current_time;
    xSemaphoreGive(s_state_mutex);
}

void state_mark_dirty(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    s_dirty = true;
    s_dirty_generation++;
    xSemaphoreGive(s_state_mutex);
}

void state_save_locked(void)
{
    if (g_users_mutex == NULL) {
        return;
    }

    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    if (!s_dirty) {
        xSemaphoreGive(s_state_mutex);
        return;
    }
    uint32_t generation = s_dirty_generation;
    state_blob_t state = {
        .version = STATE_VERSION,
        .sentinel = STATE_VALID_SENTINEL,
        .open_min = s_open_min,
        .close_min = s_close_min,
        .current_time = s_current_time,
        .timeout_seconds = s_timeout_seconds,
        .emergency = s_emergency,
        .n_users = g_n_users,
        .last_save_time = time(NULL),
    };
    memcpy(state.admin_pin, s_admin_pin, sizeof(state.admin_pin));
    memcpy(state.users, g_users, sizeof(state.users));
    xSemaphoreGive(s_state_mutex);
    uint32_t crc = state_crc(&state);

    /*
     * NVS writes are deliberately limited to user/admin mutations, not scan
     * polling. A commit while the caller holds g_users_mutex is safe because
     * no logger or access lock is taken here; callers must keep mutations
     * infrequent to limit flash wear.
     */
    nvs_handle_t handle = 0;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    bool saved = nvs_open(STATE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK &&
                 nvs_set_blob(handle, "state", &state, sizeof(state)) == ESP_OK &&
                 nvs_set_u32(handle, "crc", crc) == ESP_OK &&
                 nvs_commit(handle) == ESP_OK;
    if (handle != 0) {
        nvs_close(handle);
    }
    xSemaphoreGive(s_nvs_mutex);
    if (saved) {
        xSemaphoreTake(s_state_mutex, portMAX_DELAY);
        if (s_dirty_generation == generation) {
            s_dirty = false;
        }
        s_loaded = true;
        s_last_save_time = time(NULL);
        xSemaphoreGive(s_state_mutex);
    }
}

void state_save(void)
{
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    state_mark_dirty();
    state_save_locked();
    xSemaphoreGive(g_users_mutex);
}

void state_factory_reset(void)
{
    nvs_handle_t handle = 0;
    xSemaphoreTake(s_nvs_mutex, portMAX_DELAY);
    if (nvs_open(STATE_NAMESPACE, NVS_READWRITE, &handle) == ESP_OK) {
        nvs_erase_all(handle);
        nvs_commit(handle);
        nvs_close(handle);
    }
    xSemaphoreGive(s_nvs_mutex);
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    set_defaults();
    s_dirty = false;
    s_last_save_time = 0;
    xSemaphoreGive(s_state_mutex);
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    memset(g_users, 0, sizeof(g_users));
    g_n_users = 0;
    xSemaphoreGive(g_users_mutex);
    log_event(LOG_EVENT_SYSTEM, "state factory reset");
}

time_t state_last_save_time(void)
{
    xSemaphoreTake(s_state_mutex, portMAX_DELAY);
    time_t timestamp = s_last_save_time;
    xSemaphoreGive(s_state_mutex);
    return timestamp;
}
