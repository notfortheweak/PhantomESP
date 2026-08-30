// scan_report.c — see header. Live adapters normalize each engine's data into a
// compact record; the session accumulator (fed by the scheduler task) remembers
// everything seen so the UI shows active-vs-total over an area. The UI reads
// ONLY the accumulator, never the live engines — so the LVGL task never races
// the scheduler's scans.
#include "gui/scan_report.h"

#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

#include "esp_wifi_types.h"
#include "core/callbacks.h"                    // pineap_get_*
#include "managers/wifi_manager.h"             // station_ap_pair_t
#include "scans/wifi/camera_detect.h"
#include "core/network_constants.h"
#include "managers/aerial_detector_manager.h"
#include "managers/flock_detector_manager.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/station_scan.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/device_detect_scan.h"
#endif

#define MAC6 "%02X:%02X:%02X:%02X:%02X:%02X"
#define MACB(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

static void set_mac(char *dst, size_t n, const uint8_t *m) { snprintf(dst, n, MAC6, MACB(m)); }

// Session-stable AP IDs: each unique AP BSSID keeps the same "#N" all session,
// so a station can reference its associated AP by a short, stable number.
#define AP_ID_MAX 48
static uint8_t s_ap_bssids[AP_ID_MAX][6];
static int s_ap_id_n = 0;

static int ap_id_lookup(const uint8_t *bssid) {
    for (int i = 0; i < s_ap_id_n; i++)
        if (memcmp(s_ap_bssids[i], bssid, 6) == 0) return i;
    return -1;
}
static int ap_id_for(const uint8_t *bssid) {   // assign-or-get
    int id = ap_id_lookup(bssid);
    if (id >= 0) return id;
    if (s_ap_id_n >= AP_ID_MAX) return -1;
    memcpy(s_ap_bssids[s_ap_id_n], bssid, 6);
    return s_ap_id_n++;
}

// ---------------------------------------------------------------------------
// Live adapters
// ---------------------------------------------------------------------------

static bool resolve_ap_ssid(const uint8_t *bssid, char *out, size_t n) {
    uint16_t cnt = 0; wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&cnt, &aps);
    if (!aps) return false;
    for (int i = 0; i < (int)cnt; i++) {
        if (memcmp(aps[i].bssid, bssid, 6) == 0 && aps[i].ssid[0]) {
            snprintf(out, n, "%s", (const char *)aps[i].ssid);
            return true;
        }
    }
    return false;
}

// ---- WiFi: access points (blue) then stations (red) ----
static int wifi_count(void) { return (int)ap_scan_get_count() + station_scan_get_count(); }
static bool wifi_get(int i, scan_sig_t *o) {
    int nap = (int)ap_scan_get_count();
    if (i < nap) {
        uint16_t n = 0; wifi_ap_record_t *aps = NULL;
        ap_scan_get_results(&n, &aps);
        if (!aps || i >= (int)n) return false;
        const wifi_ap_record_t *a = &aps[i];
        int id = ap_id_for(a->bssid);
        snprintf(o->title, sizeof(o->title), "%s", a->ssid[0] ? (const char *)a->ssid : "(hidden)");
        set_mac(o->addr, sizeof(o->addr), a->bssid);
        if (id >= 0) snprintf(o->sub, sizeof(o->sub), "AP #%d  CH %d", id, a->primary);
        else         snprintf(o->sub, sizeof(o->sub), "AP  CH %d", a->primary);
        o->rssi = a->rssi; o->has_rssi = true; o->kind = SKIND_AP;
        return true;
    }
    int s = i - nap;
    if (s < 0 || s >= station_scan_get_count()) return false;
    const station_ap_pair_t *st = &station_ap_list[s];
    char ap_ssid[33];
    bool have = resolve_ap_ssid(st->ap_bssid, ap_ssid, sizeof(ap_ssid));
    int ap_id = ap_id_lookup(st->ap_bssid);   // AP already numbered this session?
    set_mac(o->title, sizeof(o->title), st->station_mac);
    set_mac(o->addr, sizeof(o->addr), st->ap_bssid);   // associated AP BSSID
    if (ap_id >= 0 && have) snprintf(o->sub, sizeof(o->sub), "STA->AP#%d %.13s", ap_id, ap_ssid);
    else if (ap_id >= 0)    snprintf(o->sub, sizeof(o->sub), "STA->AP#%d", ap_id);
    else if (have)          snprintf(o->sub, sizeof(o->sub), "STA-> %.20s", ap_ssid);
    else                    snprintf(o->sub, sizeof(o->sub), "STA-> AP (unlisted)");
    o->has_rssi = false; o->kind = SKIND_STATION;
    return true;
}

// ---- Drones ----
static int drones_count(void) { return aerial_detector_get_device_count(); }
static bool drones_get(int i, scan_sig_t *o) {
    AerialDevice *d = aerial_detector_get_device(i);
    if (!d) return false;
    snprintf(o->title, sizeof(o->title), "%s", d->vendor[0] ? d->vendor : "Drone");
    snprintf(o->addr, sizeof(o->addr), "%s", d->mac);
    snprintf(o->sub, sizeof(o->sub), "%s", aerial_detector_get_type_string(d->type));
    o->rssi = d->rssi; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

// ---- Flock ----
static int flock_count(void) { return flock_detector_get_count(); }
static bool flock_get(int i, scan_sig_t *o) {
    const FlockDetection *d = flock_detector_get_detection(i);
    if (!d) return false;
    snprintf(o->title, sizeof(o->title), "%s", d->mac);
    snprintf(o->addr, sizeof(o->addr), "%s", d->mac);
    snprintf(o->sub, sizeof(o->sub), "%s", d->method);
    o->rssi = d->rssi; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

// ---- Surveillance cameras (vendor-OUI matched; no dedicated radio phase) ----
static int cameras_count(void) { return camera_detect_get_count(); }
static bool cameras_get(int i, scan_sig_t *o) {
    const camera_detection_t *d = camera_detect_get(i);
    if (!d) return false;
    snprintf(o->title, sizeof(o->title), "%s", d->vendor ? d->vendor : "Camera");
    snprintf(o->addr, sizeof(o->addr), "%02x:%02x:%02x:%02x:%02x:%02x",
             d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
    // Tier drives both the label and the row color: targeted platforms (ALPR /
    // bodycam / cloud surveillance) matter far more than an ordinary shop camera.
    if (d->tier == SURV_TIER_TARGETED)
        snprintf(o->sub, sizeof(o->sub), "SURVEILLANCE  ch%d", d->channel);
    else
        snprintf(o->sub, sizeof(o->sub), "IP camera  ch%d", d->channel);
    o->rssi = d->rssi; o->has_rssi = (d->rssi != 0);
    o->kind = (d->tier == SURV_TIER_TARGETED) ? SKIND_STATION : SKIND_AP;
    return true;
}

// ---- PineAP ----
static int pineap_count(void) { return pineap_get_detected_count(); }
static bool pineap_get(int i, scan_sig_t *o) {
    uint8_t b[6]; int sc = 0; int8_t r = 0, ch = 0; char ssid[33] = {0};
    if (pineap_get_network_data(i, b, &sc, &r, &ch, ssid, sizeof(ssid)) != 0) return false;
    set_mac(o->title, sizeof(o->title), b);
    set_mac(o->addr, sizeof(o->addr), b);
    snprintf(o->sub, sizeof(o->sub), "%d SSIDs  CH %d", sc, ch);
    o->rssi = r; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
// ---- Flipper Zero ----
static int flipper_count(void) { return flipper_scan_get_count(); }
static bool flipper_get(int i, scan_sig_t *o) {
    uint8_t mac[6]; int8_t r = 0; char name[32] = {0};
    if (flipper_scan_get_device_data(i, mac, &r, name, sizeof(name)) != 0) return false;
    snprintf(o->title, sizeof(o->title), "%s", name[0] ? name : "Flipper");
    set_mac(o->addr, sizeof(o->addr), mac);
    snprintf(o->sub, sizeof(o->sub), "Flipper Zero");
    o->rssi = r; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

// ---- Apple AirTags ----
static int airtag_count(void) { return airtag_scan_get_count(); }
static bool airtag_get(int i, scan_sig_t *o) {
    uint8_t mac[6]; int8_t r = 0;
    if (airtag_scan_get_device_data(i, mac, &r) != 0) return false;
    set_mac(o->title, sizeof(o->title), mac);
    set_mac(o->addr, sizeof(o->addr), mac);
    snprintf(o->sub, sizeof(o->sub), "AirTag");
    o->rssi = r; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

// ---- BLE (excludes Flippers/AirTags — they have their own tiles) ----
static bool ble_is_excluded(const BLEDetectDeviceInfo *info) {
    return info->type == BLE_DETECT_DEVICE_AIRTAG ||
           info->type == BLE_DETECT_DEVICE_FLIPPER;
}
static int ble_count(void) {
    int total = ble_device_detect_get_count(), c = 0;
    for (int i = 0; i < total; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) == 0 && !ble_is_excluded(&info)) c++;
    }
    return c;
}
static bool ble_get(int idx, scan_sig_t *o) {
    int total = ble_device_detect_get_count(), c = 0;
    for (int i = 0; i < total; i++) {
        BLEDetectDeviceInfo info;
        if (ble_device_detect_get_device(i, &info) != 0) continue;
        if (ble_is_excluded(&info)) continue;
        if (c++ != idx) continue;
        const char *type = ble_device_detect_type_to_string(info.type);
        snprintf(o->title, sizeof(o->title), "%s", info.name[0] ? info.name : type);
        set_mac(o->addr, sizeof(o->addr), info.mac);
        snprintf(o->sub, sizeof(o->sub), "%s", info.subtype[0] ? info.subtype : type);
        o->rssi = info.rssi; o->has_rssi = true; o->kind = SKIND_DEFAULT;
        return true;
    }
    return false;
}
#endif // !S2

static const scan_category_t s_categories[SCAT_COUNT] = {
    [SCAT_WIFI]     = { "WiFi",     wifi_count,   wifi_get },
    [SCAT_DRONES]   = { "Drones",   drones_count, drones_get },
    [SCAT_FLOCK]    = { "Flock Cam",flock_count,  flock_get },
    [SCAT_CAMERAS]  = { "Cameras",  cameras_count,cameras_get },
    [SCAT_PINEAP]   = { "PineAP",   pineap_count, pineap_get },
#ifndef CONFIG_IDF_TARGET_ESP32S2
    [SCAT_FLIPPERS] = { "Flippers", flipper_count,flipper_get },
    [SCAT_AIRTAGS]  = { "AirTags",  airtag_count, airtag_get },
    [SCAT_BLE]      = { "BLE",      ble_count,    ble_get },
#else
    [SCAT_FLIPPERS] = { "Flippers", NULL, NULL },
    [SCAT_AIRTAGS]  = { "AirTags",  NULL, NULL },
    [SCAT_BLE]      = { "BLE",      NULL, NULL },
#endif
};

const scan_category_t *scan_report_category(scan_category_id_t id) {
    if (id < 0 || id >= SCAT_COUNT) return NULL;
    return &s_categories[id];
}

// ---------------------------------------------------------------------------
// Session accumulator (compact; UI reads this, scheduler writes it)
// ---------------------------------------------------------------------------
// Per-category depth of the session list. This is static BSS (link-time), sized
// to comfortably fit alongside runtime heap even on the no-PSRAM CYD (~42 KB
// internal free measured with this in place).
#define SEEN_MAX 20

typedef struct {
    char        title[34];
    char        addr[20];
    char        sub[36];
    int8_t      rssi;
    bool        has_rssi;
    scan_kind_t kind;
    bool        active;
} seen_t;

static seen_t s_seen[SCAT_COUNT][SEEN_MAX];
static int    s_seen_n[SCAT_COUNT];

void scan_report_reset_session(void) {
    memset(s_seen, 0, sizeof(s_seen));
    memset(s_seen_n, 0, sizeof(s_seen_n));
    s_ap_id_n = 0;   // restart AP numbering
}

// Two observations are the same device when their kind-appropriate UNIQUE key
// matches. APs are unique by BSSID (addr), not SSID (title) — keying by SSID
// collapses same-SSID or "(hidden)" APs into one row, which strands the AP#N a
// station references by BSSID. Stations are unique by their own MAC (title, since
// a station row's addr is the *shared* associated-AP BSSID). Everything else is
// unique by MAC (addr) when present, else by name (title).
static bool same_signal(const seen_t *e, const scan_sig_t *sig) {
    if (e->kind != sig->kind) return false;
    switch (sig->kind) {
    case SKIND_AP:
        return strncmp(e->addr, sig->addr, sizeof(e->addr)) == 0;
    case SKIND_STATION:
        return strncmp(e->title, sig->title, sizeof(e->title)) == 0;
    default:
        if (sig->addr[0])
            return strncmp(e->addr, sig->addr, sizeof(e->addr)) == 0;
        return strncmp(e->title, sig->title, sizeof(e->title)) == 0;
    }
}

void scan_report_accumulate(scan_category_id_t id) {
    const scan_category_t *cat = scan_report_category(id);
    if (!cat || !cat->count || !cat->get) return;

    for (int j = 0; j < s_seen_n[id]; j++) s_seen[id][j].active = false;

    int n = cat->count();
    for (int i = 0; i < n; i++) {
        scan_sig_t sig;
        memset(&sig, 0, sizeof(sig));
        if (!cat->get(i, &sig)) continue;
        int f = -1;
        for (int j = 0; j < s_seen_n[id]; j++) {
            if (same_signal(&s_seen[id][j], &sig)) { f = j; break; }
        }
        if (f < 0) {
            if (s_seen_n[id] >= SEEN_MAX) continue;
            f = s_seen_n[id]++;
        }
        seen_t *e = &s_seen[id][f];
        snprintf(e->title, sizeof(e->title), "%s", sig.title);
        snprintf(e->addr, sizeof(e->addr), "%s", sig.addr);
        snprintf(e->sub, sizeof(e->sub), "%s", sig.sub);
        e->rssi = sig.rssi; e->has_rssi = sig.has_rssi; e->kind = sig.kind;
        e->active = true;
    }
}

int scan_report_total_count(scan_category_id_t id) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    return s_seen_n[id];
}
int scan_report_active_count(scan_category_id_t id) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    int c = 0;
    for (int j = 0; j < s_seen_n[id]; j++) if (s_seen[id][j].active) c++;
    return c;
}
int scan_report_kind_active(scan_category_id_t id, scan_kind_t kind) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    int c = 0;
    for (int j = 0; j < s_seen_n[id]; j++)
        if (s_seen[id][j].active && s_seen[id][j].kind == kind) c++;
    return c;
}

bool scan_report_seen_get(scan_category_id_t id, int index, scan_sig_t *out, bool *active) {
    if (id < 0 || id >= SCAT_COUNT || !out) return false;
    int total = s_seen_n[id], c = 0;
    for (int pass = 0; pass < 2; pass++) {          // active rows first
        bool want_active = (pass == 0);
        for (int j = 0; j < total; j++) {
            seen_t *e = &s_seen[id][j];
            if (e->active != want_active) continue;
            if (c++ != index) continue;
            snprintf(out->title, sizeof(out->title), "%s", e->title);
            snprintf(out->addr, sizeof(out->addr), "%s", e->addr);
            snprintf(out->sub, sizeof(out->sub), "%s", e->sub);
            out->rssi = e->rssi; out->has_rssi = e->has_rssi; out->kind = e->kind;
            if (active) *active = e->active;
            return true;
        }
    }
    return false;
}
