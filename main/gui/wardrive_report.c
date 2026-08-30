// wardrive_report.c — see header. Fixed-slot ring of deduped wardriven networks,
// heap-allocated (PSRAM-preferred) only while the dashboard is open.
#include "gui/wardrive_report.h"

#include <string.h>
#include <stdio.h>
#include "esp_heap_caps.h"
#include "esp_timer.h"

#define WD_CAP        48       // distinct networks shown; CSV keeps the rest
#define WD_ACTIVE_MS  20000    // "active" = heard within this window

static wd_seen_t *s_buf = NULL;
static uint32_t   s_logged = 0;   // all records logged this session
static uint32_t   s_evict = 0;    // ring cursor for eviction when full

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void wardrive_report_alloc(void) {
    if (s_buf) return;
    s_buf = heap_caps_calloc(WD_CAP, sizeof(wd_seen_t),
                             MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf) s_buf = heap_caps_calloc(WD_CAP, sizeof(wd_seen_t),
                                         MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_logged = 0;
    s_evict = 0;
}

void wardrive_report_free(void) {
    if (s_buf) { heap_caps_free(s_buf); s_buf = NULL; }
    s_logged = 0;
    s_evict = 0;
}

void wardrive_report_reset(void) {
    if (s_buf) memset(s_buf, 0, WD_CAP * sizeof(wd_seen_t));
    s_logged = 0;
    s_evict = 0;
}

static bool addr_is_real(const char *a) {
    return a && a[0] && strcmp(a, "00:00:00:00:00:00") != 0;
}

// Find the slot for (kind, addr): an existing match, else a free slot, else the
// ring-evict slot. Returns NULL only when the buffer isn't allocated.
static wd_seen_t *slot_for(uint8_t kind, const char *addr) {
    if (!s_buf) return NULL;
    int free_idx = -1;
    for (int i = 0; i < WD_CAP; i++) {
        wd_seen_t *e = &s_buf[i];
        if (e->used) {
            if (e->kind == kind && strncmp(e->addr, addr, sizeof(e->addr)) == 0) return e;
        } else if (free_idx < 0) {
            free_idx = i;
        }
    }
    if (free_idx >= 0) return &s_buf[free_idx];
    wd_seen_t *e = &s_buf[s_evict % WD_CAP];
    s_evict++;
    return e;
}

void wardrive_report_accumulate(const wardriving_data_t *rec) {
    if (!s_buf || !rec) return;

    bool is_ble = addr_is_real(rec->ble_data.ble_mac) && !addr_is_real(rec->bssid);
    const char *addr = is_ble ? rec->ble_data.ble_mac : rec->bssid;
    if (!addr_is_real(addr)) return;

    wd_seen_t *e = slot_for(is_ble ? WD_KIND_BLE : WD_KIND_WIFI, addr);
    if (!e) return;

    e->kind = is_ble ? WD_KIND_BLE : WD_KIND_WIFI;
    snprintf(e->addr, sizeof(e->addr), "%s", addr);
    if (is_ble) {
        snprintf(e->title, sizeof(e->title), "%s",
                 rec->ble_data.ble_name[0] ? rec->ble_data.ble_name : "(unknown)");
        e->rssi = (int8_t)rec->ble_data.ble_rssi;
        e->enc[0] = '\0';
        e->channel = 0;
    } else {
        snprintf(e->title, sizeof(e->title), "%s", rec->ssid[0] ? rec->ssid : "(hidden)");
        e->rssi = (int8_t)rec->rssi;
        snprintf(e->enc, sizeof(e->enc), "%s", rec->encryption_type);
        e->channel = (int16_t)rec->channel;
    }
    e->has_rssi = true;

    e->has_gps = rec->gps_quality.has_valid_fix;
    e->lat = (float)rec->latitude;
    e->lon = (float)rec->longitude;
    e->alt = (float)rec->altitude;
    e->sats = rec->gps_quality.satellites_used;
    e->hdop = rec->gps_quality.hdop;

    e->last_seen_ms = now_ms();
    e->used = true;
    s_logged++;
}

int wardrive_report_count(wd_kind_t kind) {
    if (!s_buf) return 0;
    int c = 0;
    for (int i = 0; i < WD_CAP; i++)
        if (s_buf[i].used && s_buf[i].kind == kind) c++;
    return c;
}

int wardrive_report_active_count(wd_kind_t kind) {
    if (!s_buf) return 0;
    uint32_t now = now_ms();
    int c = 0;
    for (int i = 0; i < WD_CAP; i++) {
        wd_seen_t *e = &s_buf[i];
        if (e->used && e->kind == kind && (now - e->last_seen_ms) < WD_ACTIVE_MS) c++;
    }
    return c;
}

uint32_t wardrive_report_logged_total(void) { return s_logged; }

bool wardrive_report_get(wd_kind_t kind, int index, wd_seen_t *out, bool *active) {
    if (!s_buf || !out) return false;
    uint32_t now = now_ms();
    int c = 0;
    for (int pass = 0; pass < 2; pass++) {           // active rows first
        bool want_active = (pass == 0);
        for (int i = 0; i < WD_CAP; i++) {
            wd_seen_t *e = &s_buf[i];
            if (!e->used || e->kind != kind) continue;
            bool is_active = (now - e->last_seen_ms) < WD_ACTIVE_MS;
            if (is_active != want_active) continue;
            if (c++ != index) continue;
            *out = *e;
            if (active) *active = is_active;
            return true;
        }
    }
    return false;
}
