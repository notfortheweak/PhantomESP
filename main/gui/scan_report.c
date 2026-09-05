// scan_report.c — see header. Live adapters normalize each engine's data into a
// compact record; the session accumulator (fed by the scheduler task) remembers
// everything seen so the UI shows active-vs-total over an area. The UI reads
// ONLY the accumulator, never the live engines — so the LVGL task never races
// the scheduler's scans.
#include "gui/scan_report.h"
#include "gui/toast.h"                          // discovery toast (#5)

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

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

// ---- WiFi access points (blue) — SCAT_WIFI ----
// APs and stations are now two independent categories/stores (SCAT_WIFI and
// SCAT_STATIONS) so a crowd of stations can never starve the AP list (and vice
// versa) under the per-category SEEN_MAX budget — the root of the old
// "AP vanished / device disappeared" bugs when both shared one 20-slot list.
static int wifi_ap_count(void) { return (int)ap_scan_get_count(); }
static bool wifi_ap_get(int i, scan_sig_t *o) {
    uint16_t n = 0; wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&n, &aps);
    if (!aps || i < 0 || i >= (int)n) return false;
    const wifi_ap_record_t *a = &aps[i];
    int id = ap_id_for(a->bssid);
    snprintf(o->title, sizeof(o->title), "%s", a->ssid[0] ? (const char *)a->ssid : "(hidden)");
    set_mac(o->addr, sizeof(o->addr), a->bssid);
    // Show the BSSID on the row: a dual-band/mesh network (and a same-name "evil
    // twin") broadcasts one SSID from several MACs, so the BSSID is what makes
    // each physical AP distinct. Rows are still deduped per-BSSID (same_signal),
    // so each MAC appears exactly once.
    if (id >= 0) snprintf(o->sub, sizeof(o->sub), "#%d CH%d %s", id, a->primary, o->addr);
    else         snprintf(o->sub, sizeof(o->sub), "CH%d %s", a->primary, o->addr);
    o->rssi = a->rssi; o->has_rssi = true; o->kind = SKIND_AP;
    return true;
}

// ---- WiFi client stations (red) — SCAT_STATIONS ----
static int wifi_sta_count(void) { return station_scan_get_count(); }
static bool wifi_sta_get(int i, scan_sig_t *o) {
    if (i < 0 || i >= station_scan_get_count()) return false;
    const station_ap_pair_t *st = &station_ap_list[i];
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
    // MAC first: with two DroneID identities for one physical aircraft a real
    // possibility (legacy DJI vendor-IE vs standards-based OpenDroneID can come
    // from different transmitter MACs), the type string alone ("DJI WiFi" on
    // both rows) can't tell two entries apart -- the MAC can.
    snprintf(o->sub, sizeof(o->sub), "%s (%s)", d->mac,
             aerial_detector_get_type_string(d->type));
    o->rssi = d->rssi; o->has_rssi = true; o->kind = SKIND_DEFAULT;
    return true;
}

// ---- Surveillance cameras ----
// One category covering both sources: vendor-OUI matches (camera_detect) and the
// Flock Safety ALPR detector's own hits. Flock keeps its dedicated engine (probe
// heuristics + its own OUI list) but reports here so there is a single place to
// look for "who is watching", with tier driving how loudly it shouts.
static int cameras_count(void) {
    return camera_detect_get_count() + flock_detector_get_count();
}
static bool cameras_get(int i, scan_sig_t *o) {
    int ncam = camera_detect_get_count();
    if (i < ncam) {
        const camera_detection_t *d = camera_detect_get(i);
        if (!d) return false;
        snprintf(o->title, sizeof(o->title), "%s", d->vendor ? d->vendor : "Camera");
        snprintf(o->addr, sizeof(o->addr), "%02x:%02x:%02x:%02x:%02x:%02x",
                 d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4], d->mac[5]);
        if (d->tier == SURV_TIER_TARGETED)
            snprintf(o->sub, sizeof(o->sub), "SURVEILLANCE  ch%d", d->channel);
        else
            snprintf(o->sub, sizeof(o->sub), "IP camera  ch%d", d->channel);
        o->rssi = d->rssi; o->has_rssi = (d->rssi != 0);
        o->kind = (d->tier == SURV_TIER_TARGETED) ? SKIND_STATION : SKIND_AP;
        return true;
    }
    // Flock ALPR hits: always the loud tier (red), labelled with how they matched.
    const FlockDetection *f = flock_detector_get_detection(i - ncam);
    if (!f) return false;
    snprintf(o->title, sizeof(o->title), "Flock ALPR");
    snprintf(o->addr, sizeof(o->addr), "%s", f->mac);
    snprintf(o->sub, sizeof(o->sub), "SURVEILLANCE  %s", f->method);
    o->rssi = f->rssi; o->has_rssi = true; o->kind = SKIND_STATION;
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
    [SCAT_WIFI]     = { "Access Points", wifi_ap_count,  wifi_ap_get },
    [SCAT_STATIONS] = { "Stations",      wifi_sta_count, wifi_sta_get },
    [SCAT_DRONES]   = { "Drones",   drones_count, drones_get },
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
    char        sub[48];
    int8_t      rssi;
    bool        has_rssi;
    scan_kind_t kind;
    bool        active;
} seen_t;

// The table is [SCAT_COUNT][SEEN_MAX] flattened, allocated only while Live Scan
// is open. Held statically it was 14,288 bytes of permanent .bss -- the single
// largest consumer in the firmware -- on boards where the LVGL pool and the BLE
// stack are fighting over the same internal heap. PSRAM-preferred with an
// internal fallback, matching wardrive_report.
//
// The scheduler task writes this while the LVGL task reads it, and the buffer is
// freed on exit from Live Scan while the scheduler may still be finishing a
// phase, so every access is behind a mutex. Uncontended takes are cheap; the
// alternative (a bare pointer) has a real use-after-free window on exit.
static seen_t          *s_seen = NULL;
static int              s_seen_n[SCAT_COUNT];
static SemaphoreHandle_t s_seen_lock = NULL;

#define SEEN_AT(id, j) (s_seen[(id) * SEEN_MAX + (j)])
#define SEEN_LOCK()    (s_seen_lock && xSemaphoreTake(s_seen_lock, pdMS_TO_TICKS(50)) == pdTRUE)
#define SEEN_UNLOCK()  xSemaphoreGive(s_seen_lock)

void scan_report_alloc(void) {
    if (!s_seen_lock) s_seen_lock = xSemaphoreCreateMutex();
    if (!s_seen_lock || s_seen) return;
    size_t bytes = (size_t)SCAT_COUNT * SEEN_MAX * sizeof(seen_t);
    s_seen = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_seen) s_seen = heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    memset(s_seen_n, 0, sizeof(s_seen_n));
    s_ap_id_n = 0;
}

void scan_report_free(void) {
    if (!s_seen_lock) return;
    if (!SEEN_LOCK()) return;          // busy: leave it rather than free under a writer
    seen_t *tmp = s_seen;
    s_seen = NULL;                     // publish NULL before releasing
    memset(s_seen_n, 0, sizeof(s_seen_n));
    SEEN_UNLOCK();
    if (tmp) heap_caps_free(tmp);
}

void scan_report_reset_session(void) {
    if (!SEEN_LOCK()) return;
    if (s_seen) memset(s_seen, 0, (size_t)SCAT_COUNT * SEEN_MAX * sizeof(seen_t));
    memset(s_seen_n, 0, sizeof(s_seen_n));
    s_ap_id_n = 0;   // restart AP numbering
    SEEN_UNLOCK();
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

// #5: when a genuinely new device is discovered, toast WHAT it is (the type)
// instead of the old generic "Scan saved". Singular label carries an identifier
// (SSID / device id / MAC); a burst in one pass collapses to "N new <plural>".
// Called AFTER the accumulator lock is released (toast_post mallocs + defers to
// the LVGL task, so it must not run under SEEN_LOCK).
static void notify_new_devices(scan_category_id_t id, int new_count, const char *last_title) {
    if (new_count <= 0) return;
    const char *one = "device", *many = "devices";
    switch (id) {
    case SCAT_WIFI:     one = "AP";         many = "APs";         break;
    case SCAT_STATIONS: one = "station";    many = "stations";    break;
    case SCAT_DRONES:   one = "drone";      many = "drones";      break;
    case SCAT_CAMERAS:  one = "camera";     many = "cameras";     break;
    case SCAT_PINEAP:   one = "rogue AP";   many = "rogue APs";   break;
    case SCAT_FLIPPERS: one = "Flipper";    many = "Flippers";    break;
    case SCAT_AIRTAGS:  one = "AirTag";     many = "AirTags";     break;
    case SCAT_BLE:      one = "BLE device"; many = "BLE devices"; break;
    default: break;
    }
    // Threat categories get the amber toast; presence categories the info toast.
    uint8_t type = (id == SCAT_DRONES || id == SCAT_CAMERAS || id == SCAT_PINEAP ||
                    id == SCAT_FLIPPERS || id == SCAT_AIRTAGS) ? TOAST_WARN : TOAST_INFO;
    char msg[TOAST_MAX_TEXT_LEN + 1];
    if (new_count > 1)
        snprintf(msg, sizeof(msg), "%d new %s", new_count, many);
    else if (last_title && last_title[0])
        snprintf(msg, sizeof(msg), "%s: %.40s", one, last_title);
    else
        snprintf(msg, sizeof(msg), "New %s", one);
    toast_show(msg, type);
}

void scan_report_accumulate(scan_category_id_t id) {
    const scan_category_t *cat = scan_report_category(id);
    if (!cat || !cat->count || !cat->get) return;
    if (id < 0 || id >= SCAT_COUNT) return;
    if (!SEEN_LOCK()) return;
    if (!s_seen) { SEEN_UNLOCK(); return; }

    for (int j = 0; j < s_seen_n[id]; j++) SEEN_AT(id, j).active = false;

    int new_count = 0;
    char last_new_title[sizeof(((seen_t *)0)->title)] = {0};

    int n = cat->count();
    for (int i = 0; i < n; i++) {
        scan_sig_t sig;
        memset(&sig, 0, sizeof(sig));
        if (!cat->get(i, &sig)) continue;
        int f = -1;
        for (int j = 0; j < s_seen_n[id]; j++) {
            if (same_signal(&SEEN_AT(id, j), &sig)) { f = j; break; }
        }
        bool is_new = (f < 0);
        if (is_new) {
            if (s_seen_n[id] < SEEN_MAX) {
                f = s_seen_n[id]++;
            } else {
                // Store full: evict the oldest INACTIVE (stale) slot so a real
                // new device is never dropped in favor of gear that has left.
                // Entries are roughly insertion-ordered, so the first inactive
                // found is approximately the oldest. If every slot is active we
                // are genuinely at capacity — keep what we have, skip the new one.
                int victim = -1;
                for (int j = 0; j < s_seen_n[id]; j++) {
                    if (!SEEN_AT(id, j).active) { victim = j; break; }
                }
                if (victim < 0) continue;
                f = victim;
            }
            new_count++;
            snprintf(last_new_title, sizeof(last_new_title), "%s", sig.title);
        }
        seen_t *e = &SEEN_AT(id, f);
        snprintf(e->title, sizeof(e->title), "%s", sig.title);
        snprintf(e->addr, sizeof(e->addr), "%s", sig.addr);
        snprintf(e->sub, sizeof(e->sub), "%s", sig.sub);
        e->rssi = sig.rssi; e->has_rssi = sig.has_rssi; e->kind = sig.kind;
        e->active = true;
    }
    SEEN_UNLOCK();

    notify_new_devices(id, new_count, last_new_title);
}

int scan_report_total_count(scan_category_id_t id) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    if (!SEEN_LOCK()) return 0;
    int n = s_seen ? s_seen_n[id] : 0;
    SEEN_UNLOCK();
    return n;
}
int scan_report_active_count(scan_category_id_t id) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    if (!SEEN_LOCK()) return 0;
    int c = 0;
    if (s_seen)
        for (int j = 0; j < s_seen_n[id]; j++) if (SEEN_AT(id, j).active) c++;
    SEEN_UNLOCK();
    return c;
}
int scan_report_kind_active(scan_category_id_t id, scan_kind_t kind) {
    if (id < 0 || id >= SCAT_COUNT) return 0;
    if (!SEEN_LOCK()) return 0;
    int c = 0;
    if (s_seen)
        for (int j = 0; j < s_seen_n[id]; j++)
            if (SEEN_AT(id, j).active && SEEN_AT(id, j).kind == kind) c++;
    SEEN_UNLOCK();
    return c;
}

bool scan_report_seen_get(scan_category_id_t id, int index, scan_sig_t *out, bool *active) {
    if (id < 0 || id >= SCAT_COUNT || !out) return false;
    if (!SEEN_LOCK()) return false;
    if (!s_seen) { SEEN_UNLOCK(); return false; }
    int total = s_seen_n[id], c = 0;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {   // active rows first
        bool want_active = (pass == 0);
        for (int j = 0; j < total; j++) {
            seen_t *e = &SEEN_AT(id, j);
            if (e->active != want_active) continue;
            if (c++ != index) continue;
            snprintf(out->title, sizeof(out->title), "%s", e->title);
            snprintf(out->addr, sizeof(out->addr), "%s", e->addr);
            snprintf(out->sub, sizeof(out->sub), "%s", e->sub);
            out->rssi = e->rssi; out->has_rssi = e->has_rssi; out->kind = e->kind;
            if (active) *active = e->active;
            found = true;
            break;
        }
    }
    SEEN_UNLOCK();
    return found;
}
