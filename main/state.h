#pragma once

#include <stdbool.h>

void state_load(void);
void state_log_boot(void);

/*
 * Caller MUST hold g_users_mutex. This function performs the infrequent NVS
 * commit for a known state mutation; it must never be called from a fast loop.
 */
void state_save_locked(void);
void state_save(void);
void state_mark_dirty(void);
void state_factory_reset(void);
void state_update_access(int open_min, int close_min, bool emergency);
void state_update_admin(const char pin[9], unsigned int timeout_seconds,
                        int current_time);

bool state_get_loaded_access(int *open_min, int *close_min, bool *emergency);
bool state_get_loaded_admin(char pin[9], unsigned int *timeout_seconds,
                            int *current_time, bool *emergency);
