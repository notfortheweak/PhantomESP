// scan_report.h — category adapter + session accumulator for the Live Scan UI.
//
// Adapters bridge the volatile per-engine getters to a normalized signal. The
// accumulator remembers everything seen this session (active vs total) so the
// dashboard and lists reflect what's around over a whole area, not just the
// instant reading. The scan scheduler feeds it via scan_report_accumulate().
#ifndef SCAN_REPORT_H
#define SCAN_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCAT_WIFI = 0,   // access points + associated stations
    SCAT_DRONES,
    SCAT_CAMERAS,    // all surveillance cameras: vendor-OUI matches plus
                     // Flock Safety ALPR hits from the flock detector
    SCAT_PINEAP,
    SCAT_FLIPPERS,
    SCAT_AIRTAGS,
    SCAT_BLE,
    SCAT_COUNT
} scan_category_id_t;

// Row "kind" drives list/row coloring (AP=blue, Station=red, others=default).
typedef enum { SKIND_DEFAULT = 0, SKIND_AP, SKIND_STATION } scan_kind_t;

// Row colors (0xRRGGBB). Chosen distinct from the severity palette
// (green/amber/red) and the RSSI colors.
#define SCAN_COLOR_AP      0x448AFF   // blue   — access points
#define SCAN_COLOR_STATION 0xFF5252   // red    — stations
#define SCAN_COLOR_TOTAL   0xB388FF   // purple — "total seen" counters

// Compact record (no cached detail string — the detail view formats from these
// fields, keeping the session accumulator small enough for no-PSRAM boards).
typedef struct {
    char        title[34];    // primary id / dedup key (SSID / device name / MAC)
    char        addr[20];     // MAC / BSSID string ("" if none)
    char        sub[36];      // secondary (type / assoc AP #id / channel / method)
    int8_t      rssi;
    bool        has_rssi;
    scan_kind_t kind;
} scan_sig_t;

typedef struct {
    const char *name;                        // human label, e.g. "WiFi"
    int  (*count)(void);                      // live signals right now
    bool (*get)(int index, scan_sig_t *out);  // fill live signal `index`
} scan_category_t;

const scan_category_t *scan_report_category(scan_category_id_t id);

// ---- session accumulator ----
// Called by the scheduler right after a category's scan window (before the
// engine frees its data). Marks currently-seen signals active, remembers new
// ones as part of the running session total.
void scan_report_accumulate(scan_category_id_t id);
void scan_report_reset_session(void);          // forget everything (new session)

// The session table is heap-backed and lives only while Live Scan is open --
// statically it was the firmware's largest single .bss consumer. Call alloc()
// before starting the scheduler and free() after stopping it; every accessor is
// a safe no-op while unallocated.
void scan_report_alloc(void);
void scan_report_free(void);

int  scan_report_active_count(scan_category_id_t id);   // seen in latest scan
int  scan_report_total_count(scan_category_id_t id);    // unique seen this session
int  scan_report_kind_active(scan_category_id_t id, scan_kind_t kind);

// Iterate the accumulated (session) list for a category — active first.
// Fills `out` (detail regenerated live for active rows, summarized otherwise)
// and sets *active. Returns false when `index` is past the end.
bool scan_report_seen_get(scan_category_id_t id, int index, scan_sig_t *out, bool *active);

#endif // SCAN_REPORT_H
