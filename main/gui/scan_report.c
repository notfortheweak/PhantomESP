// scan_report.c — see header. Each adapter reads its engine's live list and
// normalizes one entry into scan_sig_t. Volatile engines (data freed between
// scheduler phases) simply return false when a slot is unavailable; the list
// view's snapshot preserves what was already seen.
#include "gui/scan_report.h"

#include "sdkconfig.h"
#include <stdio.h>
#include <string.h>

#include "esp_wifi_types.h"
#include "core/callbacks.h"                    // pineap_get_*
#include "managers/aerial_detector_manager.h"
#include "managers/flock_detector_manager.h"
#include "scans/wifi/ap_scan.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/device_detect_scan.h"
#endif

#define MAC6 "%02X:%02X:%02X:%02X:%02X:%02X"
#define MACB(m) (m)[0],(m)[1],(m)[2],(m)[3],(m)[4],(m)[5]

// ---- WiFi access points ----
static int wifi_count(void) { return (int)ap_scan_get_count(); }
static bool wifi_get(int i, scan_sig_t *o) {
    uint16_t n = 0; wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&n, &aps);
    if (!aps || i < 0 || i >= (int)n) return false;
    const wifi_ap_record_t *a = &aps[i];
    char ssid[33];
    snprintf(ssid, sizeof(ssid), "%s", a->ssid[0] ? (const char *)a->ssid : "(hidden)");
    snprintf(o->title, sizeof(o->title), "%s", ssid);
    snprintf(o->sub, sizeof(o->sub), "CH %d", a->primary);
    o->rssi = a->rssi; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "SSID: %s\nBSSID: " MAC6 "\nRSSI: %d dBm\nChannel: %d",
             ssid, MACB(a->bssid), a->rssi, a->primary);
    return true;
}

// ---- Drones (aerial RemoteID / DJI) ----
static int drones_count(void) { return aerial_detector_get_device_count(); }
static bool drones_get(int i, scan_sig_t *o) {
    AerialDevice *d = aerial_detector_get_device(i);
    if (!d) return false;
    const char *type = aerial_detector_get_type_string(d->type);
    snprintf(o->title, sizeof(o->title), "%s", d->vendor[0] ? d->vendor : "Drone");
    snprintf(o->sub, sizeof(o->sub), "%s", type);
    o->rssi = d->rssi; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "ID: %s\nMAC: %s\nType: %s\nRSSI: %d dBm\nCH: %d\nLat: %.5f\nLon: %.5f",
             d->device_id, d->mac, type, d->rssi, d->channel, d->latitude, d->longitude);
    return true;
}

// ---- Flock / surveillance cameras ----
static int flock_count(void) { return flock_detector_get_count(); }
static bool flock_get(int i, scan_sig_t *o) {
    const FlockDetection *d = flock_detector_get_detection(i);
    if (!d) return false;
    snprintf(o->title, sizeof(o->title), "%s", d->mac);
    snprintf(o->sub, sizeof(o->sub), "%s", d->method);
    o->rssi = d->rssi; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "MAC: %s\nMethod: %s\nRSSI: %d dBm\nCH: %d\nSSID: %s",
             d->mac, d->method, d->rssi, d->channel, d->ssid[0] ? d->ssid : "-");
    return true;
}

// ---- PineAP / rogue APs ----
static int pineap_count(void) { return pineap_get_detected_count(); }
static bool pineap_get(int i, scan_sig_t *o) {
    uint8_t b[6]; int sc = 0; int8_t r = 0, ch = 0; char ssid[33] = {0};
    if (pineap_get_network_data(i, b, &sc, &r, &ch, ssid, sizeof(ssid)) != 0) return false;
    snprintf(o->title, sizeof(o->title), MAC6, MACB(b));
    snprintf(o->sub, sizeof(o->sub), "%d SSIDs", sc);
    o->rssi = r; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "BSSID: " MAC6 "\nSSIDs seen: %d\nLast SSID: %s\nRSSI: %d dBm\nCH: %d",
             MACB(b), sc, ssid[0] ? ssid : "-", r, ch);
    return true;
}

#ifndef CONFIG_IDF_TARGET_ESP32S2
// ---- Flipper Zero ----
static int flipper_count(void) { return flipper_scan_get_count(); }
static bool flipper_get(int i, scan_sig_t *o) {
    uint8_t mac[6]; int8_t r = 0; char name[32] = {0};
    if (flipper_scan_get_device_data(i, mac, &r, name, sizeof(name)) != 0) return false;
    snprintf(o->title, sizeof(o->title), "%s", name[0] ? name : "Flipper");
    snprintf(o->sub, sizeof(o->sub), MAC6, MACB(mac));
    o->rssi = r; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "Name: %s\nMAC: " MAC6 "\nRSSI: %d dBm",
             name[0] ? name : "-", MACB(mac), r);
    return true;
}

// ---- Apple AirTags ----
static int airtag_count(void) { return airtag_scan_get_count(); }
static bool airtag_get(int i, scan_sig_t *o) {
    uint8_t mac[6]; int8_t r = 0;
    if (airtag_scan_get_device_data(i, mac, &r) != 0) return false;
    snprintf(o->title, sizeof(o->title), MAC6, MACB(mac));
    snprintf(o->sub, sizeof(o->sub), "AirTag");
    o->rssi = r; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "Apple AirTag\nMAC: " MAC6 "\nRSSI: %d dBm", MACB(mac), r);
    return true;
}

// ---- BLE devices ----
static int ble_count(void) { return ble_device_detect_get_count(); }
static bool ble_get(int i, scan_sig_t *o) {
    BLEDetectDeviceInfo info;
    if (ble_device_detect_get_device(i, &info) != 0) return false;
    const char *type = ble_device_detect_type_to_string(info.type);
    snprintf(o->title, sizeof(o->title), "%s",
             info.name[0] ? info.name : type);
    snprintf(o->sub, sizeof(o->sub), "%s",
             info.subtype[0] ? info.subtype : type);
    o->rssi = info.rssi; o->has_rssi = true;
    snprintf(o->detail, sizeof(o->detail),
             "Name: %s\nType: %s\nSubtype: %s\nMAC: " MAC6 "\nRSSI: %d dBm",
             info.name[0] ? info.name : "-", type,
             info.subtype[0] ? info.subtype : "-", MACB(info.mac), info.rssi);
    return true;
}
#endif // !S2

static const scan_category_t s_categories[SCAT_COUNT] = {
    [SCAT_WIFI]     = { "WiFi",     wifi_count,   wifi_get },
    [SCAT_DRONES]   = { "Drones",   drones_count, drones_get },
    [SCAT_FLOCK]    = { "Flock Cam",flock_count,  flock_get },
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
