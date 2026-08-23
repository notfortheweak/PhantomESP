// scan_report.h — category adapter layer for the Live Scan reporting UI.
//
// Bridges the volatile per-engine getters (ap_scan, aerial, flock, pineap,
// flipper, airtag, ble_device_detect) to a normalized signal record the list
// and detail views render. The views keep their own snapshot, so adapters only
// need to expose "how many right now" + "fill signal i".
#ifndef SCAN_REPORT_H
#define SCAN_REPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    SCAT_WIFI = 0,
    SCAT_DRONES,
    SCAT_FLOCK,
    SCAT_PINEAP,
    SCAT_FLIPPERS,
    SCAT_AIRTAGS,
    SCAT_BLE,
    SCAT_COUNT
} scan_category_id_t;

// One normalized signal for display. `title` is the dedup key used by the
// snapshot (SSID / MAC / name), so it must be stable for a given device.
typedef struct {
    char   title[34];    // primary identifier
    char   sub[28];      // secondary (vendor / type / method / channel)
    char   detail[208];  // full multi-line detail text
    int8_t rssi;
    bool   has_rssi;
} scan_sig_t;

typedef struct {
    const char *name;                        // human label, e.g. "WiFi"
    int  (*count)(void);                     // signals available right now
    bool (*get)(int index, scan_sig_t *out); // fill signal `index`; false if gone
} scan_category_t;

// Returns the adapter for `id`, or NULL if out of range.
const scan_category_t *scan_report_category(scan_category_id_t id);

#endif // SCAN_REPORT_H
