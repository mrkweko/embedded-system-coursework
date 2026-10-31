#include "state.h"

#include <stdint.h>
#include <string.h>

#include "nvs.h"
#include "nvs_flash.h"

#include "admin.h"
#include "freertos/semphr.h"
#include "shared.h"
#include "storage.h"

#define STATE_NAMESPACE "cocis_state"
#define STATE_VERSION 1U
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
static int s_loaded_records;
static nvs_handle_t s_nvs;

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
    if (err == ESP_OK &&
        nvs_open(STATE_NAMESPACE, NVS_READWRITE, &s_nvs) == ESP_OK &&
        nvs_get_blob(s_nvs, "state", &state, &size) == ESP_OK &&
        nvs_get_u32(s_nvs, "crc", &crc) == ESP_OK &&
        valid_blob(&state, size, crc)) {
        s_open_min = state.open_min;
        s_close_min = state.close_min;
        s_current_time = state.current_time;
        s_timeout_seconds = state.timeout_seconds;
        s_emergency = state.emergency;
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

    if (s_nvs != 0) {
        nvs_close(s_nvs);
        s_nvs = 0;
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
    *open_min = s_open_min;
    *close_min = s_close_min;
    *emergency = s_emergency;
    return s_loaded;
}

bool state_get_loaded_admin(char pin[9], unsigned int *timeout_seconds,
                            int *current_time, bool *emergency)
{
    if (!pin || !timeout_seconds || !current_time || !emergency) {
        return false;
    }
    strlcpy(pin, s_admin_pin, 9);
    *timeout_seconds = s_timeout_seconds;
    *current_time = s_current_time;
    *emergency = s_emergency;
    return s_loaded;
}

void state_update_access(int open_min, int close_min, bool emergency)
{
    s_open_min = open_min;
    s_close_min = close_min;
    s_emergency = emergency;
}

void state_update_admin(const char pin[9], unsigned int timeout_seconds,
                        int current_time)
{
    if (pin != NULL) {
        strlcpy(s_admin_pin, pin, sizeof(s_admin_pin));
    }
    s_timeout_seconds = timeout_seconds;
    s_current_time = current_time;
}

void state_mark_dirty(void)
{
    s_dirty = true;
}

void state_save_locked(void)
{
    if (!s_dirty || g_users_mutex == NULL) {
        return;
    }

    state_blob_t state = {
        .version = STATE_VERSION,
        .sentinel = STATE_VALID_SENTINEL,
        .open_min = s_open_min,
        .close_min = s_close_min,
        .current_time = s_current_time,
        .timeout_seconds = s_timeout_seconds,
        .emergency = s_emergency,
        .n_users = g_n_users,
    };
    unsigned int timeout_seconds;
    int current_time;
    admin_state_snapshot(state.admin_pin, &timeout_seconds, &current_time);
    state.timeout_seconds = timeout_seconds;
    state.current_time = current_time;
    s_timeout_seconds = timeout_seconds;
    s_current_time = current_time;
    memcpy(state.users, g_users, sizeof(state.users));
    uint32_t crc = state_crc(&state);

    /*
     * NVS writes are deliberately limited to user/admin mutations, not scan
     * polling. A commit while the caller holds g_users_mutex is safe because
     * no logger or access lock is taken here; callers must keep mutations
     * infrequent to limit flash wear.
     */
    if (nvs_open(STATE_NAMESPACE, NVS_READWRITE, &s_nvs) == ESP_OK &&
        nvs_set_blob(s_nvs, "state", &state, sizeof(state)) == ESP_OK &&
        nvs_set_u32(s_nvs, "crc", crc) == ESP_OK &&
        nvs_commit(s_nvs) == ESP_OK) {
        s_dirty = false;
        s_loaded = true;
    }
    if (s_nvs != 0) {
        nvs_close(s_nvs);
        s_nvs = 0;
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
    if (nvs_open(STATE_NAMESPACE, NVS_READWRITE, &s_nvs) == ESP_OK) {
        nvs_erase_all(s_nvs);
        nvs_commit(s_nvs);
        nvs_close(s_nvs);
        s_nvs = 0;
    }
    set_defaults();
    xSemaphoreTake(g_users_mutex, portMAX_DELAY);
    memset(g_users, 0, sizeof(g_users));
    g_n_users = 0;
    xSemaphoreGive(g_users_mutex);
    log_event(LOG_EVENT_SYSTEM, "state factory reset");
}
