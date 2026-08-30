// camera_detect.c — see header. Fixed-size store, no dynamic allocation, so the
// observe path stays safe to call from the promiscuous sniffer's ISR context.
#include "scans/wifi/camera_detect.h"

#include <string.h>

#include "core/network_constants.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/station_scan.h"
#include "managers/wifi_manager.h"   // station_ap_pair_t / station_ap_list
#include "esp_wifi.h"

static camera_detection_t s_dets[CAMERA_MAX_DETECTIONS];
static volatile int s_count = 0;

void camera_detect_reset(void) {
    memset(s_dets, 0, sizeof(s_dets));
    s_count = 0;
}

bool camera_detect_observe(const uint8_t *mac, int8_t rssi, uint8_t channel) {
    if (!mac) return false;

    const char *vendor = NULL;
    uint8_t tier = 0;
    if (!surveil_oui_lookup(mac, &vendor, &tier)) return false;
    if (tier == SURV_TIER_DRONE) return false;   // drones belong to the aerial detector

    // Refresh an existing sighting (dedup by MAC) ...
    int n = s_count;
    if (n > CAMERA_MAX_DETECTIONS) n = CAMERA_MAX_DETECTIONS;
    for (int i = 0; i < n; i++) {
        if (s_dets[i].used && memcmp(s_dets[i].mac, mac, 6) == 0) {
            s_dets[i].rssi = rssi;
            s_dets[i].channel = channel;
            return true;
        }
    }
    // ... otherwise append while there is room. Full is fine: the tile still
    // reflects presence, and a fixed cap keeps this allocation-free.
    if (n >= CAMERA_MAX_DETECTIONS) return true;

    camera_detection_t *d = &s_dets[n];
    memcpy(d->mac, mac, 6);
    d->vendor = vendor;          // static string, safe to alias
    d->tier = tier;
    d->rssi = rssi;
    d->channel = channel;
    d->used = true;
    s_count = n + 1;             // publish last so readers never see a partial row
    return true;
}

void camera_detect_sweep_wifi_lists(void) {
    // Access points seen by the regular WiFi scan.
    uint16_t ap_n = 0;
    wifi_ap_record_t *aps = NULL;
    ap_scan_get_results(&ap_n, &aps);
    if (aps) {
        for (int i = 0; i < (int)ap_n; i++) {
            camera_detect_observe(aps[i].bssid, (int8_t)aps[i].rssi, aps[i].primary);
        }
    }

    // Client stations — a WiFi camera associated to a router shows up here.
    int st_n = station_scan_get_count();
    for (int i = 0; i < st_n; i++) {
        const station_ap_pair_t *st = &station_ap_list[i];
        camera_detect_observe(st->station_mac, 0, 0);
    }
}

int camera_detect_get_count(void) {
    int n = s_count;
    return n > CAMERA_MAX_DETECTIONS ? CAMERA_MAX_DETECTIONS : n;
}

const camera_detection_t *camera_detect_get(int index) {
    if (index < 0 || index >= camera_detect_get_count()) return NULL;
    return &s_dets[index];
}

int camera_detect_get_targeted_count(void) {
    int n = camera_detect_get_count(), c = 0;
    for (int i = 0; i < n; i++) {
        if (s_dets[i].used && s_dets[i].tier == SURV_TIER_TARGETED) c++;
    }
    return c;
}
