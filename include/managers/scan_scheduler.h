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

// Focus scanning on a single category (a scan_category_id_t) so it refreshes
// rapidly instead of waiting for the full round-robin — used when the user
// drills into a category. Pass -1 to resume the normal all-category rotation.
// Category currently being scanned (scan_category_id_t), or -1 between phases.
// The dashboard pulses the matching tile so an empty tile is distinguishable
// from one that simply has not been scanned yet.
int scan_scheduler_current_category(void);

void scan_scheduler_set_focus(int category);

#ifdef __cplusplus
}
#endif
