# Member 3 implementation handoff

## Repository reality

- This is an ESP-IDF C project, not an Arduino project with `sketch.ino`. The
  application entry point is [`main/main.c`](../main/main.c); the component
  sources are `main.c`, `console.c`, `admin.c`, `access.c`, and `rfid.c`
  ([`main/CMakeLists.txt`](../main/CMakeLists.txt)).
- [`diagram.json`](../diagram.json) currently contains one ESP32, two MFRC522
  readers, and entry/exit LEDs on GPIO 25/26. There is no SD-card part or
  wiring. No project documentation beyond source comments/configuration was
  found.
- Current `HEAD` is the 31-Oct baseline checkpoint. Recent history is mainly
  by nushifah (Member 4 console/shared/status work), NSG1414
  (Member 2 RFID/access-hours work), and Vee (admin work). The worktree was
  clean before this file was added. Relevant commits include `cebd2a9`
  (log/rooms weak hooks), `6ac5699` (shared users/`who`), `dc2d7d3`
  (shared users and access schedule), and `c2ff74b` (remove temporary access
  stub).

## Actual sections and runtime objects

| Object | Definition/use | Configuration and protection |
|---|---|---|
| `scan_queue` | [`main.c:16-16`](../main/main.c#L16), consumed by `access_task` | `xQueueCreate(8, sizeof(scan_event_t))`; `access_task` blocks forever on receive ([`access.c:165-174`](../main/access.c#L165)); console and RFID producers use zero-wait send ([`console.c:154-159`](../main/console.c#L154), [`rfid.c:176-177`](../main/rfid.c#L176)). |
| `g_users`, `g_n_users` | [`main.c:18-19`](../main/main.c#L18), shared contract [`shared.h:8-27`](../main/shared.h#L8) | `g_users_mutex` must protect every read/write. `user_t.inside` is changed only by `handle_scan`; admin add/delete and console readers also use the mutex. |
| `g_users_mutex` | Created in [`main.c:24-24`](../main/main.c#L24) | Mutex for the user table/count. All current users-table paths take it with `portMAX_DELAY`; print after releasing it. |
| `s_mutex` | [`access.c:21-24`](../main/access.c#L21) | Mutex for access `s_open_min`, `s_close_min`, and `s_emergency`; access getters/setters and scan decision use it. It is not shared with admin's duplicate schedule variables. |
| `console_task` | [`console.c:193-224`](../main/console.c#L193) | `xTaskCreate(..., "console", 6144, NULL, 3, NULL)`; no core affinity (scheduler may place it on either core). |
| `access_task` | [`access.c:165-190`](../main/access.c#L165) | `xTaskCreate(..., "access", 4096, NULL, 5, NULL)`; no core affinity. Handles scans and holds no mutex during printing or the 2-second door delay. |
| `rfid_task` | [`rfid.c:157-190`](../main/rfid.c#L157) | `xTaskCreate(..., "rfid", 4096, NULL, 4, NULL)`; no core affinity. Its reader state is task-local by ownership, but SPI calls can run concurrently with future SD code. |
| `admin_monitor_task` | [`main.c:33-34`](../main/main.c#L33), implementation [`admin.c:472-485`](../main/admin.c#L472) | `xTaskCreate(..., "admin_monitor", 3072, NULL, 2, NULL)`; no core affinity. |

There are no room or storage/logger tasks currently created. Planned section
names in the prompt do not match this checkout: the actual access logic is
`handle_scan`/`access_task` in `access.c`; console is the actual Section 7;
admin is in `admin.c`; RFID polling is in `rfid.c`.

## Logging and output

- Access decisions print `[ACCESS] ... DENIED/GRANTED` in
  [`access.c:157-163`](../main/access.c#L157).
- RFID card detection prints `[RFID] ...` in [`rfid.c:176-176`](../main/rfid.c#L176)
  and chip diagnostics use `ESP_LOGI` at [`rfid.c:151-153`](../main/rfid.c#L151).
- Console help, status, who, rooms, scan-submission/errors, prompt/echo, and
  unknown-command output are all `printf` calls in [`console.c:25-224`](../main/console.c#L25).
- Admin login/logout, PIN, schedule, simulated time, users, timeout,
  emergency, settings, and expiry all print directly in
  [`admin.c:60-485`](../main/admin.c#L60).
- `cmd_log` prints a heading then calls `logger_print_recent(n)` at
  [`console.c:101-107`](../main/console.c#L101). The weak fallback at
  [`console.c:93-98`](../main/console.c#L93) only says storage is unavailable.
  `logger_dropped_count()` is also a weak `-1` fallback at
  [`console.c:110-111`](../main/console.c#L110).
- The RFID producer ignores the return value of its nonblocking queue send;
  queue-full events are therefore currently silently lost.

## Persistence, hardware, and state

- There is no SD, Preferences/NVS, EEPROM, or RTC implementation in this
  checkout. `access.c` uses libc time and a temporary simulated clock
  (`setenv`, `tzset`, `settimeofday`) at [`access.c:107-118`](../main/access.c#L107);
  this is not persistent RTC state.
- Runtime user state is the shared `g_users[MAX_USERS]` table and
  `g_n_users`: UID, name, and `inside` flag ([`shared.h:15-20`](../main/shared.h#L15)).
  RFID scans seed five test users only when the table is empty
  ([`access.c:83-104`](../main/access.c#L83)). Admin add/delete mutates the same
  table ([`admin.c:213-296`](../main/admin.c#L213)).
- Access settings are duplicated: real enforcement uses `s_open_min`,
  `s_close_min`, `s_emergency` in [`access.c:21-24`](../main/access.c#L21);
  admin separately keeps `opening_time`, `closing_time`, `current_time`, and
  `emergency_active` ([`admin.c:27-39`](../main/admin.c#L27)). Admin schedule
  and emergency commands do not call the access setters, so displayed/admin
  settings can disagree with enforcement. The admin PIN, login state,
  failed attempts/lockout, timeout, schedule, simulated time, and emergency
  flag are all RAM-only.
- `access_is_open`, `access_set_hours`, `access_get_hours`,
  `access_set_emergency`, and `access_schedule_state` are the existing public
  schedule APIs in [`access.h:6-14`](../main/access.h#L6).

## Member 3 scope: missing or incorrect

1. Add the storage module and `storage_start()` integration; currently
   `main.c:43` only has the commented `// storage_start();`.
2. Add an SD-card part and wiring to [`diagram.json`](../diagram.json), while
   coordinating SPI ownership with the MFRC522 bus (`rfid.c:187-188`).
3. Implement a bounded log queue and `logger_task`. Producers must use a
   zero-wait send and increment an atomic/appropriately protected dropped-event
   counter on failure; never block access, RFID, room, or CLI producers.
4. Implement the existing console hook signatures exactly:
   `void logger_print_recent(int n)` and `int logger_dropped_count(void)`
   ([`console.c:93-111`](../main/console.c#L93)). Add the real definitions to
   the component so they override the weak fallbacks, and ensure the `log`
   command reads the SD-backed/ring-buffer data rather than the fallback text.
5. Add persistent state load/save for users and settings. The requested
   `state_save_locked` symbol does not exist in this checkout; establish the
   integration contract as `void state_save_locked(void)` called only while
   `g_users_mutex` is held (it must not take that mutex again), or document a
   different explicit lock contract before implementation.
6. Persist at least the shared users/count and each `inside` value (“who is
   inside”), plus the effective access settings and admin settings required by
   the project. Load before `access_start()` seeds defaults, otherwise the
   seed path can mask restored data.

## Risks to resolve (do not silently inherit)

- `xQueueSend(..., 0)` is intentionally nonblocking, but console reports
  “queue full” while RFID silently drops. Log queue behavior must be separate
  from scan queue behavior and must expose drops.
- `admin.c` has no mutex around its many static globals. `console_task` and
  `admin_monitor_task` can race on login, timeout, lockout, PIN, schedule, and
  emergency state. Admin schedule/emergency also race conceptually with the
  independent access mutex/state.
- `g_users_mutex` is taken with `portMAX_DELAY`; a slow future persistence
  operation or SD I/O must not occur while holding it. Existing code generally
  copies/releases before printing, but `state_save_locked` must be designed
  carefully to avoid lock-held I/O and deadlock.
- `handle_scan` snapshots access rules under `s_mutex`, releases it, then
  updates users under `g_users_mutex`; an admin can change rules between those
  operations. This is a consistency window, not currently a deadlock.
- `access_task` blocks indefinitely on `scan_queue`; that is expected consumer
  behavior, but any producer using a blocking send could stall behind it.
- `access_task` holds no lock during `vTaskDelay(2000 ms)`, but serial `printf`
  from multiple tasks can interleave. SD writes and `logger_print_recent`
  need serialization with the logger/SD owner.
- `rfid_start` initializes SPI and ignores most driver return codes; the comment
  says SD may start the bus first, but there is no shared SPI mutex or actual SD
  initialization. Two independent bus initialization paths are unsafe.
- `admin_start()` is called after `g_users_mutex` creation but before other
  tasks; it only checks that mutex and returns an error that `app_main` ignores.
- `admin_monitor_task` is declared/started even though the task's shared admin
  state is unprotected. `current_time` is only displayed and does not control
  `time(NULL)` access decisions.

## Ordered implementation plan

1. Agree and document the ownership contract: `g_users_mutex` protects the
   user table/count; one effective settings mutex protects schedule/emergency;
   SD access has one owner/mutex; no mutex is held during printf or SD I/O.
2. Define the log record shape and bounded queue/drop-counter API, implement
   nonblocking enqueue helpers, and add `logger_task` with explicit queue-full
   accounting.
3. Add SD hardware/wiring and initialize it once, resolving SPI bus sharing
   with RFID; add mount/write/read error reporting without success-shaped
   fallbacks.
4. Implement `storage_start`, `logger_print_recent`, and
   `logger_dropped_count`; replace the weak console behavior through normal
   linking and verify `log [n]` plus status dropped count.
5. Implement state load/save and `state_save_locked` with a snapshot-before-I/O
   design. Load persisted users/settings before `access_start`; save after
   successful mutations and on the required settings changes.
6. Reconcile admin's duplicate schedule/emergency/PIN settings with the
   effective access state, and explicitly decide which values are persisted;
   do not leave admin display and access enforcement divergent.
7. Update `main.c`, component registration, and `diagram.json`; build and test
   queue saturation, reboot restoration, log retrieval, SD failure, and
   concurrent scan/admin operations in Wokwi.

## Session 2

### Logging implementation

- Added [`storage.h`](../main/storage.h) and [`storage.c`](../main/storage.c).
  `storage_start(void)` creates the logging queue and logger task.
- Event API: `bool log_event(log_event_type_t type, const char *fmt, ...)`.
  It formats into the fixed `LOG_MESSAGE_SIZE` (80-byte) buffer and always
  sends with zero timeout. `LOG_QUEUE_LENGTH` is 32: it absorbs short bursts
  without making producers wait, while the bounded ring remains the user-facing
  history. Failed sends increment `s_dropped_events` under
  `s_status_lock`.
- Event types are `LOG_EVENT_ACCESS_GRANTED`, `LOG_EVENT_ACCESS_DENIED`, and
  `LOG_EVENT_RFID_SCAN`. `log_entry_t` contains `time_t timestamp`,
  `TickType_t uptime_ticks`, the enum, and the fixed message buffer. The
  current project has no hardware RTC; timestamps use the existing libc
  `time(NULL)`/`localtime_r` path, which is initialized by the simulated clock
  in `access.c`. Before that clock is valid, output uses uptime as a
  `1970-01-01 HH:MM:SS` fallback. A real RTC integration must replace this
  fallback before deployment.
- `logger_task` is the only SD-touching task. It mounts `/sd`, appends
  `events.csv`, flushes and closes every line, marks `sd_ok` false on errors,
  unmounts failed media, and retries every `SD_RETRY_TICKS` (10 seconds).
  Public APIs are `int log_get_recent(int n, log_entry_t *out)`,
  `uint32_t log_dropped_count(void)`, and `bool log_sd_ok(void)`.
  `log_get_recent` clamps to the available entries and 20-entry ring size and
  copies oldest-first under the ring mutex. Console compatibility wrappers are
  `void logger_print_recent(int n)` and `int logger_dropped_count(void)`.
- SD uses dedicated CS GPIO 32. SPI is shared with both MFRC522 readers on SCK GPIO 18,
  MOSI GPIO 23, and MISO GPIO 19; reader CS pins remain GPIO 5 and 17, and
  reader reset pins remain GPIO 21 and 16. The SD module is wired to those
  pins in [`diagram.json`](../diagram.json). GPIO 25/26 remain the door LEDs.
- `s_ring_mutex` protects the 20-entry RAM ring. `s_status_lock` protects
  `s_dropped_events` and `s_sd_ok`. `s_log_queue` is owned by the logger
  implementation and FreeRTOS protects queue operations. No task holds one of
  these locks while holding `g_users_mutex` or access `s_mutex`.
- `logger_task(void *arg)` is private to `storage.c`; the component explicitly
  requires `driver`, `fatfs`, and `sdmmc` for the SD/SPI APIs. Messages are
  sanitized before writing so commas/newlines cannot break the one-event-per-
  CSV-line format.

## Session 3

### NVS persistence

- Added [`state.h`](../main/state.h) and [`state.c`](../main/state.c), using
  ESP-IDF NVS because the repository has no Arduino `Preferences` or other
  persistence backend. Namespace: `cocis_state`.
- Keys:
  - `state`: one `state_blob_t` NVS blob.
  - `crc`: FNV-1a-style 32-bit checksum of the complete blob.
  - The blob also contains `version = 1` and sentinel
    `0xC0C15A7E`; size, version, sentinel, field ranges, PIN termination, and
    CRC are all validated before use.
- Blob layout: access `open_min`, `close_min`, `emergency`; admin
  `timeout_seconds` (the only existing light/automatic timeout setting),
  `current_time`, and `admin_pin[9]`; `n_users`; and all `MAX_USERS` fixed-size
  `user_t` records including UID/tag, name, and `inside`.
- Defaults on first boot, invalid NVS, or CRC/sentinel failure are 07:00 to
  20:00, emergency off, timeout 120 seconds, simulated time 08:00, admin PIN
  `1234`, and an empty user table. `access.c` then seeds the five Wokwi users.
  There are no per-user PIN fields in the current data model, so user PINs
  cannot be persisted until that model is added.
- `state_load(void)` is called once in [`main.c`](../main/main.c) immediately
  after `g_users_mutex` and `scan_queue` creation and before any task starts.
  It restores `g_users`/`g_n_users` and caches settings for `admin_start()` and
  `access_start()`. `state_log_boot(void)` is called after the logger task is
  started and queues a SYSTEM event stating `restored` or `defaulted` and the
  restored record count.
- `state_save_locked(void)` has the exact contract that the caller already
  holds `g_users_mutex`; it does not take that mutex. It snapshots users and
  commits the validated blob. `state_save(void)` is the wrapper for changes
  made outside that lock. `state_mark_dirty(void)` gates commits. Writes are
  only triggered by mutations, never the scan/polling loops, to limit NVS flash
  wear. NVS commit is intentionally infrequent; the current implementation
  performs it synchronously while the state mutex is held, so future work
  should batch/defer if command latency becomes unacceptable.
- Reboot semantic: the persisted `inside` flags are restored exactly. A reboot
  while people are inside does not silently zero occupancy; `who` reports the
  restored occupants until a valid exit scan changes each flag.
- `state_factory_reset(void)` erases the namespace, resets RAM users/settings
  to defaults, and queues a SYSTEM event. It is intentionally not exposed as a
  command here; Members 4/5 must add a CLI command and ensure it is invoked
  only by an authenticated administrator.

### Persistence hooks

- [`access.c`](../main/access.c): after a successful entry/exit mutation,
  `state_mark_dirty(); state_save_locked(); // [kweko] persist occupancy`
  runs while `g_users_mutex` is held.
- [`admin.c`](../main/admin.c): schedule set calls `access_set_hours`, updates
  the state cache, and calls `state_save()`; PIN set, simulated time set,
  timeout set, and emergency on/off each update the cache and call
  `state_save()`. User add/delete call `state_mark_dirty()` and
  `state_save_locked()` while holding `g_users_mutex`. Every addition is
  marked `// [kweko] persist`.
- [`access.c`](../main/access.c) updates the state cache whenever its schedule
  setters run; this keeps the effective access schedule aligned with the
  admin settings before the next save.
- `state_update_admin(...)` and `state_update_access(...)` maintain the
  private cached settings used by `state_save_locked()`;
  `state_get_loaded_access(...)` and `state_get_loaded_admin(...)` transfer
  validated boot values to the owning modules. NVS snapshots no longer call
  into admin code, avoiding a cross-module admin-state race.

### Wokwi reboot test

1. Start Wokwi with the added microSD wiring and open the serial monitor.
2. Log in, run `schedule set 09:00 18:00`, `timeout 300`, and
   `pin set 5678`; add a user with `user add <UID> Test`.
3. Use a valid entry scan for a known user, confirm `who` shows that user
   inside, and wait for the SYSTEM boot/event log if needed.
4. Reset the ESP32 without issuing an exit scan.
5. Confirm the boot SYSTEM event says `state restored`, `users` shows the
   added UID/name as INSIDE, `who` still lists the occupant, the new PIN
   authenticates, and settings retain the schedule/timeout.
6. Issue a valid exit scan and verify `who` removes the user; inspect
   `/sd/events.csv` for CSV SYSTEM/access records.
7. To test fallback, erase the Wokwi NVS/flash image or call the future
   authenticated factory-reset command, reboot, and verify `state defaulted`
   followed by the five seeded users and safe defaults.

## Session 4

### Review findings and fixes

- Fixed explicit SPI startup ordering in [`storage.c`](../main/storage.c):
  `storage_start()` initializes SPI2 with SCK 18, MISO 19, and MOSI 23 before
  the logger task attempts SD mounting. SD CS is GPIO 32; RFID CS remains GPIO
  5/17. The ESP-IDF SPI driver serializes transactions for the shared bus.
  `rfid.c` now accepts the already-initialized bus and only adds its devices.
- Fixed persistence-cache races by adding `s_state_mutex` in `state.c`.
  Access/admin cache updates, dirty state, getters, and last-save time are now
  protected. `state_save_locked()` snapshots under the state mutex, releases
  it, then performs NVS I/O while honoring its caller-held
  `g_users_mutex` contract.
- Fixed a second persistence race found during review: `state.c` now uses
  local NVS handles serialized by `s_nvs_mutex`, rather than one shared
  handle. A dirty-generation counter prevents a newer mutation from being
  cleared when an older concurrent snapshot finishes committing.
- Added `log_entries_count()` (saturating `uint32_t` counter), and
  `state_last_save_time()`. `status` now displays SD availability, entries
  received by the logger, dropped events, and the last successful save.
  `log [n]` now clamps directly to `LOG_RING_SIZE` (20), avoiding a misleading
  “last 50” heading; it still uses the existing compatibility wrapper, which
  reads only the RAM ring through `log_get_recent`.
- Added debug-only logger stack instrumentation guarded by
  `CONFIG_LOGGER_STACK_DEBUG`; it prints
  `[LOGGER] stack high-water mark: <words> words` at task startup. The logger
  task remains 4096 words. No runtime measurement was possible without the
  ESP-IDF build.
- Made the total logged and dropped counters saturate at `UINT32_MAX`; they do
  not wrap. The queue remains length 32 and all producer sends use zero wait.
- SD absence/removal remains nonfatal: the logger keeps draining into the RAM
  ring, marks `sd_ok` false after mount/write failure, and retries every
  10 seconds. Queue order is FIFO; timestamps are captured at enqueue time,
  so equal timestamps retain queue order. The logger task is the only SD
  accessor.
- Producer timestamps preserve event-capture time, while CSV line order
  preserves queue-delivery order. With multiple producers, a task preempted
  after capturing its timestamp can therefore produce a slightly older
  timestamp after a newer one; strict chronological output would require
  logger-assigned timestamps or sequence-number sorting. This is documented,
  not silently normalized.
- Versioned the persisted blob to version 2 and persisted
  `last_save_time`, so status can report the last successful save after reboot.

### Lock-acquisition graph

```text
admin command / access mutation
        |
        v
  g_users_mutex  --->  s_state_mutex  --->  (released before NVS I/O)
        |                     ^
        |                     |
        +---------------------+
        |
        +--------------------> s_nvs_mutex (NVS commit)

state cache update  --->  s_state_mutex
logger task          --->  s_ring_mutex
log_event            --->  FreeRTOS log-queue internals, then s_status_lock
status/log display   --->  g_users_mutex; then separate getter calls
```

No code path intentionally acquires `s_state_mutex` and then
`g_users_mutex`; therefore the state-cache graph has no cycle. `log_event()`
is called after access/user locks are released. NVS commit still occurs while
the caller holds `g_users_mutex` by API contract; this can cause priority
inversion/blocking for the priority-5 access task during flash I/O. It is
bounded and infrequent, but the recommended future patch is a single-slot
deferred state-writer task that commits a copied snapshot after releasing the
user mutex.

### Remaining integration issues (not changed)
- [`admin.c:21-39`](../main/admin.c#L21) and
  [`admin.c:472-485`](../main/admin.c#L472) access admin globals from both
  console and monitor tasks without a mutex. Suggested patch: add an admin
  mutex or confine all admin-state changes to one task; do not make the
  logger/state mutex protect those unrelated fields.
- [`admin.c:213-330`](../main/admin.c#L213) performs synchronous NVS commits
  while holding `g_users_mutex` through `state_save_locked()`. This is a
  priority-inversion risk, documented above; suggested patch is the deferred
  writer design.
- [`console.c:93-111`](../main/console.c#L93) retains weak compatibility
  fallbacks. They are overridden by the strong storage definitions today, but
  the preferred future patch is for Member 4 to include `storage.h` and call
  `log_get_recent`, `log_dropped_count`, and `log_sd_ok` directly.

### Wokwi safety checklist

1. **Normal logging:** start Wokwi with the SD card present; expect the boot
   event `SYSTEM,state defaulted, 0 user records` on a blank NVS, then seeded
   users. Submit `scan 01020304 in`; expect `[ACCESS] ENTRY GRANTED` and a
   CSV `ACCESS_GRANTED` line in `/sd/events.csv`. Run `log 5`; expect the
   event in oldest-first RAM order and `status` to show `SD logging: OK`.
2. **Burst of 200:** add a temporary test loop/task that calls
   `log_event(LOG_EVENT_SYSTEM, "burst %d", i)` for `i=0..199` without delay.
   The producer must return immediately; `status` must show
   `Entries logged` up to 200 plus prior events and a nonzero
   `Log events dropped` if the queue cannot drain fast enough. `log 20` must
   show no more than 20 entries and must not block on SD.
3. **No SD:** remove/comment the `wokwi-microsd-card` part/wires and reboot.
   Expect continued access operation, `status` showing `SD logging: NOT
   AVAILABLE`, and `log 20` still showing RAM events. No panic or reboot is
   expected; after reinserting the card, expect recovery within one retry
   interval after a new event.
4. **Reboot persistence:** set schedule, timeout, PIN, add a user, scan the
   user in, and confirm `who`. Reset without scanning out. Expect boot
   `SYSTEM,state restored, <n> user records`, the user still `INSIDE`, the
   changed PIN to authenticate, and the settings to remain changed. `status`
   should report the persisted last-save time.
5. **Factory defaults:** invoke the future authenticated CLI wrapper for
   `state_factory_reset()` or erase the NVS/flash image, then reboot. Expect
   `SYSTEM,state defaulted, 0 user records`, safe defaults (07:00-20:00,
   timeout 120, PIN 1234), and the five Wokwi seed users after access
   initialization. The factory-reset helper itself is implemented but no
   command was added by this task.

### Validation

- `git diff --check` and JSON syntax checks passed.
- No ESP-IDF/Arduino/PlatformIO toolchain is installed, so compile, stack
  high-water values, SPI runtime behavior, Wokwi output, and exact NVS reboot
  behavior remain unverified.

### Integration hooks and other-member edits

- [`main.c`](../main/main.c) includes `storage.h` and calls `storage_start()`
  before `access_start()`, so logging infrastructure exists before RFID/access
  tasks begin.
- [`main/CMakeLists.txt`](../main/CMakeLists.txt) registers `storage.c`.
- [`access.c`](../main/access.c) has the smallest `[kweko]` hooks after the
  existing access `printf` calls to enqueue granted/denied events. These calls
  are deliberately outside both `s_mutex` and `g_users_mutex`.
- [`rfid.c`](../main/rfid.c) has one smallest `[kweko]` hook after its existing
  RFID print to enqueue the scan event; it remains nonblocking.
- The existing weak console hooks are intentionally left in
  [`console.c`](../main/console.c); the strong implementations in `storage.c`
  override them without refactoring Member 4's file.
- `diagram.json` adds a `wokwi-microsd-card` on shared SPI and dedicated CS GPIO 32.
  No existing reader or LED pin was changed.

### Validation

- `idf.py`/Arduino CLI/PlatformIO were not available in the environment, so
  an ESP-IDF compile and Wokwi run are unverified. `git diff --check` should be
  run before handoff; the ESP-IDF target must verify the SD/FATFS API versions.

## Build fix

### Cause

The reported `esp_timer.h` and `sdmmc_host.h` failures were dependency-graph
failures, not missing SDK files. `esp_timer.h` is supplied by `esp_timer`.
The SD card uses SPI, so `storage.c` uses the `driver/sdmmc_host.h` compatibility
namespace together with `driver/sdspi_host.h`; it does not use the SDMMC bus
driver. `main/CMakeLists.txt` now names the split driver components and every
other component used by the source. Changes to files owned by other members are
marked `// [kweko] build fix`.

### Header/component audit

| Source file | Header | Providing component |
|---|---|---|
| `main.c`, `access.c` | `freertos/FreeRTOS.h`, `freertos/task.h`, `freertos/queue.h`, `freertos/semphr.h` | `freertos` |
| `console.h`, `console.c`, `admin.c`, `access.c`, `state.c`, `shared.h`, `storage.c` | `freertos/*` | `freertos` |
| `main.c`, `access.c`, `rfid.c`, `storage.c` | `driver/gpio.h` | `esp_driver_gpio` |
| `rfid.c`, `storage.c` | `driver/spi_master.h` | `esp_driver_spi` |
| `storage.c` | `driver/spi_common.h` | `esp_driver_spi` |
| `console.c` | `driver/uart.h` | `esp_driver_uart` |
| `storage.c` | `driver/sdspi_host.h` | `esp_driver_sdspi` |
| `storage.c` | `driver/sdmmc_host.h` | `esp_driver_sdmmc` |
| `storage.c` | `sdmmc_cmd.h` | `sdmmc` |
| `storage.c` | `esp_vfs_fat.h` | `fatfs` |
| `rfid.c` | `esp_timer.h` | `esp_timer` |
| `state.c` | `nvs.h`, `nvs_flash.h` | `nvs_flash` |
| `storage.c`, `admin.h` | `esp_err.h` | `esp_common` |
| `rfid.c` | `esp_log.h` | `log` |

The source has no `driver/i2c.h`, LEDC, or direct SDMMC-bus API use, so
`esp_driver_i2c` and `esp_driver_ledc` are intentionally not required. The
repository has one component (`main/`), and PlatformIO maps `src_dir = main`.
The header locations and provider CMakeLists were checked in both installed
framework trees: ESP-IDF 5.5.3 and 6.1.0.

### Supported versions and clean-build status

- ESP-IDF 6.1.0: **verified** with a clean `pio run -e esp32dev-idf61`;
  compilation and linking completed with no compiler warnings.
- ESP-IDF 5.5.3: **source compatibility checked, full build UNVERIFIED**.
  `pio run -e esp32dev-idf55` compiled the project sources but this host's
  PlatformIO environment has conflicting `kconfiglib` installations
  (`MenuNode` is missing), then fails in the generated `x509_crt_bundle.S`
  certificate-bundle step. This is a host-tool failure, not a missing-header
  or source compile failure.
- No standalone `idf.py` installation was available. Wokwi runtime behavior
  is unverified.

For a clean build, remove generated output and run:

```text
rm -rf .pio/build build
rm -f sdkconfig
pio run -e esp32dev-idf55
pio run -e esp32dev-idf61
```

For a clean committed-state clone, clone the committed branch into a
temporary directory and run the same commands. Repair/recreate the PlatformIO
ESP-IDF 5.5 Python environment before retrying that environment; do not copy
packages from the 6.1 environment.

## SD wiring/pin map

The SD card shares the SPI signals with both RFID readers but has its own chip
select. Card-detect and write-protect are not wired and are explicitly
disabled in the SD SPI configuration.

| Signal | ESP32 GPIO | Code | Diagram |
|---|---:|---|---|
| SD SCK | 18 | `SPI_SCK_PIN` | `sd:SCK` -> `esp:18` |
| SD MISO / DO | 19 | `SPI_MISO_PIN` | `sd:DO` -> `esp:19` |
| SD MOSI / DI | 23 | `SPI_MOSI_PIN` | `sd:DI` -> `esp:23` |
| SD CS | 32 | `SD_CS_PIN` | `sd:CS` -> `esp:32` |
| SD VCC | 3V3 | diagram only | `sd:VCC` -> `esp:3V3` |
| SD GND | GND | diagram only | `sd:GND` -> `esp:GND.1` |
| SD CD | not used | `SDSPI_SLOT_NO_CD` | not wired |
| SD WP | not used | `SDSPI_SLOT_NO_WP` | not wired |

`storage_start()` initializes `SPI2_HOST` exactly once before creating the
logger task; `rfid_start()` only adds the RFID devices to that existing bus.

## SD wiring fix

| Wokwi pin | ESP32 connection |
|---|---|
| CS | GPIO 32 |
| SCK | GPIO 18 |
| DO (MISO) | GPIO 19 |
| DI (MOSI) | GPIO 23 |
| VCC | 3V3 |
| GND | GND |
| CD | not wired |

Wokwi's microSD part uses `DI` and `DO`, not `MOSI` and `MISO`; the old wires
connected to nonexistent pins. The diagram now uses `sd:DI` and `sd:DO`,
matching the existing SPI code and dedicated SD CS on GPIO 32.
