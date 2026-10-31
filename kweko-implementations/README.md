# Member 3: Storage, Memory, and Synchronization Handover

## 1. Overview

This handover documents Member 3's work for the CoCIS Smart Control and
Monitoring System. The repository is an ESP-IDF C application, not an Arduino
`sketch.ino` project.

### Implemented

- A fixed-size, nonblocking logging API and FreeRTOS queue.
- A single `logger_task` that owns SD-card access.
- CSV logging to `/sd/events.csv` with flush/close after each event.
- A 20-entry RAM ring buffer for `log` without SD access.
- Saturating dropped-event and received-entry counters.
- SD availability state and periodic retry after mount/write failure.
- ESP-IDF NVS persistence for users, tags, names, occupancy, schedule,
  emergency mode, administrator PIN, timeout, simulated time, and last save
  time.
- Version, sentinel, size, range, and FNV-1a-style checksum validation.
- Boot SYSTEM event describing restored/defaulted state.
- Persistence hooks for occupancy and administrator mutations.
- Debug-only logger stack high-water instrumentation.
- SD wiring in [`diagram.json`](../diagram.json).

### Not implemented

- No per-user PIN field exists in the current `user_t`; per-user PIN
  persistence cannot be implemented without changing the shared data model.
- No factory-reset CLI command was added.
- No admin mutex was added; the existing admin task/console race remains an
  integration issue.
- The duplicate SPI initialization in [`rfid.c`](../main/rfid.c) remains.
- No deferred persistence writer was added. NVS commit remains synchronous
  while the caller holds `g_users_mutex`.
- No hardware RTC was added. The existing libc/simulated clock is used.
- No ESP-IDF build or Wokwi runtime test was possible because the toolchain was
  unavailable.

## 2. Architecture

```text
 RFID task / access task / future room task / CLI producer
                  |
                  | log_event(...), zero timeout
                  v
          s_log_queue (32 entries)
                  |
                  v
          logger_task (priority 2, stack 4096)
             |                  |
             |                  +--> s_ring_mutex --> RAM ring (20 entries)
             |
             +--> SPI2 + SD CS GPIO 4 --> /sd/events.csv
                         |
                         +-- shared SCK 18, MISO 19, MOSI 23

 console "log" --> log_get_recent() --> RAM ring, no SD wait
 console "status" --> log_sd_ok(), log_entries_count(),
                      log_dropped_count(), state_last_save_time()

 state_load() before tasks
       |
       +--> NVS namespace "cocis_state"
              keys: state blob + crc

 state mutations --> state_mark_dirty()
       |
       +--> state_save_locked() / state_save()
              --> validated snapshot --> NVS commit
```

Equivalent Mermaid view:

```mermaid
flowchart LR
  P[RFID/access/CLI producers] -->|zero-timeout log_event| Q[s_log_queue 32]
  Q --> L[logger_task]
  L --> R[s_ring_mutex / RAM ring 20]
  L -->|SPI2, CS 4| SD[/sd/events.csv]
  C[console log] -->|log_get_recent| R
  S[console status] --> G[log_sd_ok / entries / dropped / last save]
  M[state mutations] --> N[state_save_locked/state_save]
  N --> F[NVS cocis_state: state + crc]
  F -->|state_load before tasks| M
```

The MFRC522 readers share SPI2 pins with the SD card. RFID chip selects are
GPIO 5 and 17; RFID reset pins are GPIO 21 and 16. Door LEDs remain GPIO 25
and 26.

## 3. Synchronization design

### Locks and queues

| Name | Type | Protects | Who takes it | Maximum intended hold | Ordering rule |
|---|---|---|---|---|---|
| `g_users_mutex` | FreeRTOS mutex | `g_users[]`, `g_n_users` | Access, admin, console, state load/save | Short copy or mutation; NVS commit can make `state_save_locked()` long | Do not call `log_event()` while held. |
| `s_mutex` | FreeRTOS mutex | Access schedule and emergency state in `access.c` | Access getters/setters and scan decision | Only scalar read/write | Release before user lock, printing, delay, or logging. |
| `s_state_mutex` | FreeRTOS mutex | Persistence cache, dirty flag/generation, loaded flag, last-save timestamp | `state.c` getters/setters/save | Scalar copy only; released before NVS I/O | No path takes it and then `g_users_mutex`. |
| `s_nvs_mutex` | FreeRTOS mutex | NVS handle/commit serialization | `state_save_locked()`, factory reset | Entire NVS open/write/commit/close | Taken after the snapshot; no other project lock is acquired from NVS code. |
| `s_ring_mutex` | FreeRTOS mutex | 20-entry RAM log ring | Logger and `log_get_recent()` | Copy/replace fixed entries only | Never held during SD I/O or printing. |
| `s_status_lock` | FreeRTOS portMUX | Dropped count, SD flag, logged-entry count | Logger status getters and logger task | A few scalar operations | No blocking call while held. |
| `scan_queue` | FreeRTOS queue, length 8 | RFID scan events | Console/RFID send; access receive | Producers use zero timeout; access waits for work | Independent of logging. |
| `s_log_queue` | FreeRTOS queue, length 32 | `log_entry_t` values | Any task calls `log_event`; logger receives | Producer never waits; logger receives one item | Queue internals serialize access. |

### Deadlock argument

The intended graph is:

```text
g_users_mutex -> s_state_mutex -> release -> s_nvs_mutex
access s_mutex -> release -> g_users_mutex
logger queue internals -> s_status_lock
logger -> s_ring_mutex
console -> g_users_mutex, then separate getters
```

There is no intended reverse edge from `s_state_mutex` to `g_users_mutex`, and
the logger does not take the user or access mutex. Logging hooks in
[`access.c`](../main/access.c) run after both access and user locks are
released. Therefore the Member 3 lock graph has no cycle.

The important remaining timing risk is priority inversion: `state_save_locked`
performs synchronous NVS I/O while the caller still owns `g_users_mutex`.
This is infrequent but can delay the priority-5 access task. The future
hardening option is a deferred writer that receives a copied snapshot after
the user mutex is released.

The current logger stack is 4096 words. When `CONFIG_LOGGER_STACK_DEBUG` is
enabled, `logger_task` prints `uxTaskGetStackHighWaterMark(NULL)` at startup.
No measured value is available without an ESP-IDF runtime.

## 4. API reference for other members

Public declarations are in [`storage.h`](../main/storage.h) and
[`state.h`](../main/state.h).

### Logging API

```c
void storage_start(void);
bool log_event(log_event_type_t type, const char *fmt, ...);
int log_get_recent(int n, log_entry_t *out);
uint32_t log_dropped_count(void);
bool log_sd_ok(void);
uint32_t log_entries_count(void);
```

Constants:

```c
#define LOG_MESSAGE_SIZE 80
#define LOG_RING_SIZE 20
#define LOG_QUEUE_LENGTH 32
```

Event enum:

```c
typedef enum {
    LOG_EVENT_ACCESS_GRANTED = 0,
    LOG_EVENT_ACCESS_DENIED,
    LOG_EVENT_RFID_SCAN,
    LOG_EVENT_SYSTEM,
} log_event_type_t;
```

Entry structure:

```c
typedef struct {
    time_t timestamp;
    TickType_t uptime_ticks;
    log_event_type_t type;
    char message[LOG_MESSAGE_SIZE];
} log_entry_t;
```

`log_event()` is safe from normal FreeRTOS tasks, uses fixed stack/local
storage and no heap allocation in the producer path, and always uses queue
timeout zero. It returns `true` when queued and `false` when the queue is
full or not initialized. A failed send increments the saturating
`uint32_t` dropped counter.

`log_get_recent()` clamps to the available entries and `LOG_RING_SIZE`,
copies oldest-first, and takes `s_ring_mutex` internally. It does not access
the SD card. The caller must provide an output array large enough for the
requested/clamped count; the normal console path uses 20 entries.

Example for a Member 1/2 producer:

```c
log_event(LOG_EVENT_SYSTEM, "room %d changed", room_number);
log_event(LOG_EVENT_RFID_SCAN, "%s reader: card %s", "ENTRY", uid);
```

Example implementation of a direct Member 4 `log` command:

```c
log_entry_t entries[LOG_RING_SIZE];
int requested = 10;
if (requested < 1) requested = 1;
if (requested > LOG_RING_SIZE) requested = LOG_RING_SIZE;
int count = log_get_recent(requested, entries);
for (int i = 0; i < count; ++i) {
    /* Format entries[i] and print after log_get_recent has released its lock. */
}
printf("dropped=%lu sd=%s\n",
       (unsigned long)log_dropped_count(),
       log_sd_ok() ? "OK" : "NOT AVAILABLE");
```

The current console uses the strong compatibility functions
`logger_print_recent(int)` and `logger_dropped_count(void)` supplied by
`storage.c`; these are wrappers over the canonical API.

Example status fields:

```c
printf("SD: %s\n", log_sd_ok() ? "OK" : "NOT AVAILABLE");
printf("entries: %lu\n", (unsigned long)log_entries_count());
printf("dropped: %lu\n", (unsigned long)log_dropped_count());
printf("last save: %ld\n", (long)state_last_save_time());
```

### Persistence API

```c
void state_load(void);
void state_log_boot(void);
void state_save_locked(void);
void state_save(void);
void state_mark_dirty(void);
void state_factory_reset(void);
void state_update_access(int open_min, int close_min, bool emergency);
void state_update_admin(const char pin[9],
                        unsigned int timeout_seconds,
                        int current_time);
bool state_get_loaded_access(int *open_min, int *close_min, bool *emergency);
bool state_get_loaded_admin(char pin[9],
                            unsigned int *timeout_seconds,
                            int *current_time,
                            bool *emergency);
time_t state_last_save_time(void);
```

`state_load()` is setup-only and must run after `g_users_mutex` exists but
before tasks start. `state_log_boot()` must run after `storage_start()` so the
SYSTEM event can enter the logging queue.

`state_save_locked()` has a strict name-level contract: the caller must already
hold `g_users_mutex`; it does not take that mutex. It snapshots users and
cached settings, releases `s_state_mutex`, then performs serialized NVS I/O.
`state_save()` is the wrapper for mutations made without the user mutex; it
takes `g_users_mutex`, marks dirty, saves, and releases it.

### Do and don't

**Do**

- Use `log_event()` with short fixed-format messages.
- Treat a `false` return as a dropped event, not as permission to block/retry
  synchronously.
- Call `state_mark_dirty()` after a persistent mutation.
- Use `state_save_locked()` only while holding `g_users_mutex`.
- Use `log_get_recent()` for `log`; never make the CLI read the SD file.
- Keep printing and SD I/O outside data mutexes where possible.

**Don't**

- Do not call `xQueueSend()` with a nonzero wait for the logging queue.
- Do not call `state_save_locked()` without `g_users_mutex`.
- Do not call `log_event()` while holding `g_users_mutex` or `s_mutex`.
- Do not access `/sd/events.csv` from another task.
- Do not assume `log_sd_ok()` means an event was persisted; the RAM ring can
  still work when SD is absent.
- Do not add a second SPI bus initialization path without coordinating with
  the RFID owner.

### Existing `[kweko]` hooks

- [`console.c:16-17`](../main/console.c#L16): include storage/state APIs for
  status and log integration.
- [`console.c:106`](../main/console.c#L106): clamp `log [n]` to 20.
- [`console.c:127`](../main/console.c#L127): use the unsigned dropped counter.
- [`rfid.c:15`](../main/rfid.c#L15): include logging API.
- [`access.c:14`](../main/access.c#L14): include logging/persistence APIs.
- [`access.c:168`](../main/access.c#L168): persist successful occupancy changes.
- [`admin.c:192`](../main/admin.c#L192): persist administrator PIN.
- [`admin.c:219`](../main/admin.c#L219): persist schedule.
- [`admin.c:241`](../main/admin.c#L241): persist simulated time.
- [`admin.c:297`](../main/admin.c#L297): persist user registration.
- [`admin.c:330`](../main/admin.c#L330): persist user deletion.
- [`admin.c:386`](../main/admin.c#L386): persist timeout.
- [`admin.c:401`](../main/admin.c#L401) and
  [`admin.c:408`](../main/admin.c#L408): persist emergency changes.

The actual log enqueue calls are in [`access.c:165-172`](../main/access.c#L165)
and [`rfid.c:178-179`](../main/rfid.c#L178), although those calls themselves
do not carry the `[kweko]` marker.

## 5. Persistence design

ESP-IDF NVS is used because the repository has no other persistence backend.
The namespace is `cocis_state`.

Current keys:

- `state`: serialized `state_blob_t`.
- `crc`: 32-bit checksum of the complete blob.

Current blob version is `2`, with sentinel `0xC0C15A7E`. The blob contains:

- `version`, `sentinel`
- `open_min`, `close_min`, `emergency`
- `current_time`, `timeout_seconds`
- `admin_pin[9]`
- `n_users`
- `last_save_time`
- all `MAX_USERS` `user_t` records: UID, name, and `inside`

The blob is accepted only when its NVS size, version, sentinel, user count,
schedule range, timeout range, PIN termination, and checksum are valid.

Defaults are:

- Schedule: 07:00 to 20:00
- Emergency: off
- Timeout: 120 seconds
- Simulated time: 08:00
- Administrator PIN: `1234`
- Empty user table before `access.c` seeds its five Wokwi users

Writes occur on mutations rather than in polling loops. This reduces flash
wear. The current trade-off is that a mutation can synchronously wait for NVS
commit while holding `g_users_mutex`; the design notes this as a future
deferred-writer target.

The `inside` flags are restored exactly. A reboot does not silently clear the
occupancy list. The person remains visible to `who` until a valid exit scan
changes the flag.

## 6. Design decisions and trade-offs

### Queue plus single writer

Producers format a fixed record and perform a zero-timeout queue send. Only
`logger_task` touches the SD filesystem. This avoids concurrent file handles,
keeps producers independent of slow SD latency, and makes SD failure handling
centralized.

Rejected alternative: each producer opening and writing the file. That would
increase contention, create ordering/race problems, and allow SD delays to
block access or room tasks.

### RAM ring plus SD

The RAM ring makes `log` responsive and usable without an SD card. SD provides
durable history beyond the 20-entry interactive view.

Rejected alternative: reading SD for every `log` command. It would block the
console on media latency and fail unnecessarily when the card is absent.

### NVS for state

NVS is already provided by ESP-IDF, supports small key/value/blob state, and
survives reset. A version, sentinel, and checksum protect against blank or
corrupt state.

Rejected alternative: EEPROM or a second SD state file. Neither exists in the
project, and an SD state file would make boot state dependent on removable
media.

### Open, append, flush, close

Each event is opened, appended, flushed, and closed. This costs I/O overhead
but reduces the amount of data lost if power is removed and avoids keeping a
file handle across card removal.

### Dropped-event policy

The logging path never blocks. Queue-full events are counted, not retried
indefinitely. This preserves access-control responsiveness and exposes loss
through `log_dropped_count()`.

## 7. Code walkthrough for explanation

### `storage.h`

- Defines the three queue/ring/message constants.
- Defines the four event types, including `LOG_EVENT_SYSTEM`.
- Defines `log_entry_t` with wall-clock time, uptime fallback context, type,
  and fixed message storage.
- Declares the logger and compatibility APIs.

### `storage.c`

- Constants identify the mount point, CS pin, retry period, filename, and
  shared SPI pins.
- Static variables own the queue, ring, status counters, and mutexes.
- `event_type_name()` converts the enum to CSV text.
- `set_sd_ok()` updates the SD flag under `s_status_lock`.
- `mount_sd()` mounts FATFS through SDSPI, opens the append file as a
  readiness check, and marks the card usable only after both steps succeed.
- `unmount_sd()` releases the mounted card and clears the status.
- `add_to_ring()` writes one event into the circular RAM history.
- `format_timestamp()` uses `time_t` when the simulated clock is valid and
  otherwise formats uptime as a clearly synthetic 1970 date.
- `append_event()` sanitizes commas/newlines, writes one CSV line, flushes,
  closes, and reports failure.
- `logger_task()` receives records, adds them to RAM, increments the
  received-entry counter, retries SD mounting, and appends only from this
  task. It keeps running when SD is missing.
- `log_event()` formats into a fixed local record and sends with timeout zero.
- `log_get_recent()` clamps and copies oldest-first under the ring mutex.
- The three getters read protected counters/status.
- `logger_print_recent()` is the console compatibility wrapper.
- `storage_start()` initializes SPI2, creates the queue/ring mutex, and starts
  the logger task with priority 2 and stack 4096.

### `state.h`

Declares setup, save, reset, cache-update, cache-getter, and last-save APIs.
The comment above `state_save_locked()` states its caller-held mutex contract.

### `state.c`

- Defines the NVS namespace, version, sentinel, and safe defaults.
- `state_blob_t` is the complete persisted snapshot.
- `state_crc()` computes the fixed 32-bit checksum over the complete blob.
- `set_defaults()` initializes safe RAM state.
- `valid_blob()` rejects malformed or incompatible data.
- `state_load()` initializes NVS/mutexes, reads and validates the blob, and
  restores users before tasks begin.
- `state_log_boot()` queues the restore/default SYSTEM event after logging is
  available.
- Loaded-state getters transfer validated values to access/admin owners.
- `state_update_access()` and `state_update_admin()` update cached settings
  under `s_state_mutex`.
- `state_mark_dirty()` records a mutation and advances the generation.
- `state_save_locked()` snapshots under the cache mutex, serializes NVS
  operations with `s_nvs_mutex`, and clears dirty only if no newer generation
  appeared.
- `state_save()` supplies the user-mutex wrapper for non-locked callers.
- `state_factory_reset()` erases the namespace, resets RAM state, clears
  users, and queues a SYSTEM event.
- `state_last_save_time()` returns the last successful save timestamp.

### Integration files

- [`main.c`](../main/main.c) creates the shared user mutex and scan queue,
  loads state, starts storage, logs boot status, then starts existing tasks.
- [`access.c`](../main/access.c) persists successful occupancy transitions and
  logs granted/denied access.
- [`admin.c`](../main/admin.c) persists settings and user-table changes.
- [`console.c`](../main/console.c) clamps `log` to the RAM ring and displays
  logger/state status.
- [`rfid.c`](../main/rfid.c) logs detected cards.

### Glossary

- **Queue:** bounded FIFO buffer between producers and a consumer task.
- **Mutex:** mutual-exclusion lock allowing one task at a time into a
  protected critical section.
- **Priority inversion:** a high-priority task waits for a lock held by a
  lower-priority task, potentially delayed by medium-priority work.
- **NVS:** ESP-IDF non-volatile storage, a flash-backed key/value store.
- **SPI chip select:** per-device line selecting which SPI peripheral responds.
- **FAT/FATFS:** filesystem format and ESP-IDF filesystem layer used on SD.
- **Flash wear:** finite endurance of flash sectors; infrequent batched writes
  reduce erase/program cycles.

## 8. Test plan, results, limitations, and future work

### Planned Wokwi tests

1. **Normal logging:** boot with SD, run `scan 01020304 in`, then `log 5`.
   Expect `[ACCESS] ENTRY GRANTED`, an `ACCESS_GRANTED` CSV record, and
   `status` reporting `SD logging: OK`.
2. **Burst:** call `log_event(LOG_EVENT_SYSTEM, "burst %d", i)` for 200
   iterations without delay. The producer must return immediately; the
   dropped counter should become nonzero if the queue cannot drain fast
   enough, and `log 20` must remain bounded.
3. **No SD:** remove the SD part/wires. Access and RAM `log` must continue;
   status must report `NOT AVAILABLE`, with no panic or reboot.
4. **Reboot:** change schedule, timeout, PIN, add a user, scan in, reset, and
   confirm `SYSTEM,state restored`, the user still `INSIDE`, and settings
   remain changed.
5. **Blank/default:** erase NVS or use the future authenticated factory-reset
   command. Expect `SYSTEM,state defaulted`, safe defaults, and the five
   seeded Wokwi users after access initialization.

### Verified

- JSON syntax for [`diagram.json`](../diagram.json) was checked.
- `git diff --check` passed during the implementation sessions.
- Public symbols, constants, hooks, and line references were rechecked for
  this document.

### Unverified

- ESP-IDF compilation and warnings.
- Runtime stack high-water values.
- Wokwi SD mount/write/removal behavior.
- NVS reboot and corruption behavior on hardware/simulator.
- Exact queue saturation results for 200 events.

### Future work

- Remove or coordinate the duplicate SPI initialization in [`rfid.c`](../main/rfid.c).
- Add an admin mutex for admin state shared by console and monitor tasks.
- Replace synchronous lock-held NVS commits with a deferred snapshot writer.
- Add an authenticated factory-reset CLI command.
- Add a real RTC if calendar-accurate timestamps are required.
- Add per-user PIN fields only if the team changes `user_t`.
- Change Member 4's console implementation to call the canonical APIs directly
  instead of relying on weak compatibility wrappers.

## 9. Demo script

### Exact commands

```text
help
status
log 5
login 1234
schedule set 09:00 18:00
timeout 300
user add DEADBEEF Demo
scan 01020304 in
who
log 10
status
```

For a reboot demonstration, reset the ESP32 after `who`, then run:

```text
login 1234
who
log 10
status
scan 01020304 out
who
```

If a previous run changed the PIN, use the persisted PIN rather than assuming
`1234`.

### Two-minute narration outline

1. **0:00-0:20:** Identify [`storage.c`](../main/storage.c) as the Member 3
   logger and explain that producers never wait for SD.
2. **0:20-0:40:** Run `status` and point out SD state, entries, dropped count,
   and last NVS save.
3. **0:40-1:00:** Run a scan and `log 5`. Explain that `log` reads the
   20-entry RAM ring through `log_get_recent`, not the SD card.
4. **1:00-1:20:** Explain `s_log_queue` (32), `s_ring_mutex`, and
   `s_status_lock`; show that the logger task is the only SD writer.
5. **1:20-1:40:** Explain `g_users_mutex`, `s_state_mutex`, and `s_nvs_mutex`.
   State that `state_save_locked()` requires `g_users_mutex` and that no
   logging call occurs while it is held.
6. **1:40-2:00:** Reboot, run `who`, and show that the inside flag persisted.
   Finish by explaining that absent SD affects durability, not RAM logging or
   access control.

## 10. Likely viva questions and model answers

1. **Why is the logging queue bounded?**  
   Memory is fixed and predictable on an embedded target. A length of 32
   absorbs short bursts without permitting unbounded RAM growth.

2. **Why must producers use zero timeout?**  
   Logging must never delay access control, RFID polling, or room control.
   Full queues are counted as dropped events instead.

3. **What happens when the queue is full?**  
   `log_event()` returns `false` and increments the saturating dropped counter.
   It does not wait or retry.

4. **Why is there a separate ring buffer?**  
   The console needs a fast recent-history view that works even without SD.

5. **Who is allowed to access the SD card?**  
   Only `logger_task`; producers enqueue records and never open files.

6. **What happens if the SD card is absent?**  
   Mount fails, `sd_ok` is false, the logger remains alive, records remain in
   the RAM ring, and mounting is retried periodically.

7. **Why flush and close every line?**  
   It reduces data lost on power removal and avoids retaining a handle across
   card removal, at the cost of more filesystem overhead.

8. **How is the SPI bus shared?**  
   SCK/MISO/MOSI are shared; each device has a distinct chip-select line.
   The ESP-IDF SPI driver serializes transactions.

9. **What is the remaining SPI integration risk?**  
   `rfid.c` redundantly calls `spi_bus_initialize()` and should be coordinated
   with `storage_start()`.

10. **Which mutex protects the user table?**  
    `g_users_mutex` protects both `g_users` and `g_n_users`.

11. **What does `s_state_mutex` protect?**  
    Persistence cache values, dirty/generation state, loaded state, and the
    last-save timestamp.

12. **Why is `s_nvs_mutex` needed?**  
    It serializes NVS handle operations and commits when multiple mutation
    callers could save concurrently.

13. **Can `state_save_locked()` be called without a lock?**  
    No. Its contract requires the caller to already hold `g_users_mutex`.

14. **Can logging deadlock with persistence?**  
    Member 3 code avoids that path: access logging occurs after access/user
    locks are released, and the logger does not take the user/state locks.

15. **What is the priority-inversion concern?**  
    Synchronous NVS commit occurs while `g_users_mutex` is held, potentially
    delaying the priority-5 access task. A deferred writer is future work.

16. **How is corrupted NVS detected?**  
    The blob size, version, sentinel, ranges, PIN termination, and checksum
    must all validate.

17. **What happens to occupants after reboot?**  
    Their persisted `inside` flags are restored. Occupancy is not silently
    cleared.

18. **Why is the dropped counter saturating?**  
    It must not wrap to zero after enough failures; saturation preserves a
    truthful “at least this many” count.

19. **Are log timestamps guaranteed to be strictly chronological?**  
    Queue order is FIFO, but concurrent producers capture timestamps before
    enqueueing. A preempted producer can therefore have an older timestamp
    written later.

20. **Why use NVS instead of SD for settings?**  
    NVS is internal, available at boot, and independent of removable media.

21. **What does `log_sd_ok()` prove?**  
    It reports the logger's latest mount/write state. It does not prove that
    every queued event was persisted.

22. **What is the purpose of `uptime_ticks`?**  
    It provides a fallback ordering/time context when the simulated wall clock
    has not yet been initialized.

23. **Why is the RAM ring only 20 entries?**  
    It bounds memory while matching the intended interactive “recent events”
    view; durable history remains on SD.

24. **What should a future factory-reset command enforce?**  
    It must require authenticated admin access before calling
    `state_factory_reset()`.

## 11. Report-ready text

### Storage and synchronization

The system implements a bounded, task-safe event logging pipeline for the
ESP32. Producers call `log_event()` with a fixed-size record containing a
timestamp, event type, and short message. The function uses a FreeRTOS queue
of 32 records with zero timeout, so logging can never block access-control,
RFID, or room-control tasks. Queue-full events increment a saturating dropped
counter. A single priority-2 logger task consumes the queue and is the only
task permitted to access the microSD filesystem. It appends one sanitized CSV
record at a time to `/sd/events.csv`, flushes and closes the file, and retries
mounting after card absence or removal.

For responsive CLI access, the logger maintains a mutex-protected ring of the
20 most recent events. `log_get_recent()` copies entries oldest-first without
touching the SD card, so the `log` command remains usable when removable
storage is unavailable. The logger exposes SD status, received-entry count,
and dropped-entry count. Shared users and occupancy are protected by
`g_users_mutex`; access schedule data is protected by the access mutex.
Persistence cache values use `s_state_mutex`, while NVS operations are
serialized by `s_nvs_mutex`. Logging is invoked after access/user locks are
released, avoiding a lock cycle.

Persistent state is stored in ESP-IDF NVS as a versioned, sentinel-protected,
checksummed blob. It contains schedule, emergency mode, administrator PIN,
timeout, simulated time, user records, occupancy flags, and last save time.
Invalid data falls back to safe defaults. Successful entry/exit mutations and
administrator changes trigger saves. The persisted occupancy flags are
restored after reboot, preserving the “who is inside” view.

### Architecture and synchronization overview

The architecture separates real-time producers from slow removable-storage
I/O. RFID, access, and future room producers enqueue fixed-size log records
without waiting. The logger task drains the queue, updates a bounded RAM
history, and writes durable CSV records through one SD owner. The console
reads the RAM history and status getters rather than the SD card. SPI data
lines are shared by the SD and MFRC522 devices, with separate chip-select
lines.

Synchronization follows ownership: `g_users_mutex` protects the shared user
table, the access mutex protects effective access settings, the state mutex
protects persistence caches, and the ring/status locks protect logger-owned
state. No Member 3 path intentionally acquires these locks in reverse order.
The primary remaining timing trade-off is synchronous NVS commit while the
user mutex is held; a deferred snapshot writer is the recommended future
optimization.
