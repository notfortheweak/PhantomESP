// camera_detect.h — surveillance-camera detection store.
//
// Cameras are identified purely by vendor OUI (SURVEIL_OUIS[] in
// core/network_constants.c). There is deliberately NO dedicated radio phase:
// observations are fed from sources that already run —
//   1. the Flock detector's promiscuous sniffer (ISR context), and
//   2. the AP + station lists collected during the normal WiFi phase
// — so adding camera detection costs no extra scan time.
#ifndef CAMERA_DETECT_H
#define CAMERA_DETECT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAMERA_MAX_DETECTIONS 24
#define CAMERA_MAC_STR_LEN    18

typedef struct {
    // Raw bytes, not a formatted string: the observe path runs inside the WiFi
    // promiscuous callback, where the surrounding code is deliberately
    // snprintf-free. Callers format this when they render it.
    uint8_t     mac[6];
    const char *vendor;   // points into SURVEIL_VENDOR_NAMES (static storage)
    uint8_t     tier;     // surveil_tier_t: AMBIENT or TARGETED
    int8_t      rssi;
    uint8_t     channel;
    bool        used;
} camera_detection_t;

void camera_detect_reset(void);

// Record a sighting if `mac` belongs to a known surveillance vendor.
// Allocation-free and non-blocking: safe to call from the promiscuous sniffer
// callback (ISR context). Returns true when the MAC matched.
bool camera_detect_observe(const uint8_t *mac, int8_t rssi, uint8_t channel);

// Sweep the current AP + station scan results for camera vendors. Call from a
// normal task (the scan scheduler), never from the sniffer.
void camera_detect_sweep_wifi_lists(void);

int  camera_detect_get_count(void);
const camera_detection_t *camera_detect_get(int index);
// Number of SURV_TIER_TARGETED hits (ALPR / bodycam / cloud surveillance).
int  camera_detect_get_targeted_count(void);

#ifdef __cplusplus
}
#endif

#endif // CAMERA_DETECT_H
