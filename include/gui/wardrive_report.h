// wardrive_report.h — session accumulator for the wardriving drill-down UI.
//
// Wardriving keeps only counts + streams full records to CSV; there is no
// enumerable in-memory list to drill into. This mirrors Live Scan's scan_report:
// the scan thread feeds observations via wardrive_report_accumulate() (called
// from the CSV logging choke point, off the LVGL task), and the dashboard/list
// read them back. The buffer is heap-allocated only while the wardrive dashboard
// is open (PSRAM-preferred) so it costs nothing on the tight no-PSRAM heap when
// unused. Lock-free single-writer/single-reader with fixed compact records, like
// scan_report — a transient stale field is harmless, no OOB.
#ifndef WARDRIVE_REPORT_H
#define WARDRIVE_REPORT_H

#include <stdbool.h>
#include <stdint.h>
#include "vendor/GPS/gps_logger.h"   // wardriving_data_t

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { WD_KIND_WIFI = 0, WD_KIND_BLE } wd_kind_t;

// One deduped wardriven network (WiFi AP or BLE device) with the GPS fix where
// it was last heard. Compact: the CSV holds the full history.
typedef struct {
    char     title[34];    // SSID / BLE name
    char     addr[20];     // BSSID / BLE MAC (dedup key within kind)
    char     enc[10];      // "WPA2"/"OPEN" (WiFi); "" for BLE
    int16_t  channel;      // WiFi channel; 0 for BLE
    int8_t   rssi;
    bool     has_rssi;
    float    lat, lon, alt;
    bool     has_gps;
    uint8_t  sats;
    float    hdop;
    uint8_t  kind;         // wd_kind_t
    uint32_t last_seen_ms;
    bool     used;
} wd_seen_t;

// Buffer lifetime (dashboard create/destroy). alloc is a no-op if already present.
void wardrive_report_alloc(void);
void wardrive_report_free(void);
void wardrive_report_reset(void);   // clear entries + counters (keeps the buffer)

// Fed from the scan thread (csv_write_data_to_buffer). No-op if buffer not alloc'd.
void wardrive_report_accumulate(const wardriving_data_t *rec);

// UI reads (LVGL task).
int      wardrive_report_count(wd_kind_t kind);         // distinct seen this session
int      wardrive_report_active_count(wd_kind_t kind);  // heard in the last ~20s
uint32_t wardrive_report_logged_total(void);            // all records logged this session
// Iterate one kind, active rows first; fills out + *active. False past the end.
bool     wardrive_report_get(wd_kind_t kind, int index, wd_seen_t *out, bool *active);

#ifdef __cplusplus
}
#endif

#endif // WARDRIVE_REPORT_H
