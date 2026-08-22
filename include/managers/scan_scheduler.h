// scan_scheduler.h
// Continuous, time-sliced multi-radio detection loop for the live scan dashboard.
// One WiFi and one BLE radio exist, so detectors are run *sequentially* in short
// phases (never two promiscuous WiFi consumers at once), generalizing the proven
// `sweep` sequence into a forever loop. Each engine keeps accumulating its own
// counts; the dashboard just reads the existing *_get_count() getters.
#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start the background scheduler task (no-op if already running).
void scan_scheduler_start(void);

// Request the scheduler to stop; the task finishes its current phase, stops all
// radios, and exits. Safe to call when not running.
void scan_scheduler_stop(void);

bool scan_scheduler_is_running(void);

#ifdef __cplusplus
}
#endif
