// scan_scheduler.c — see header. Mirrors the phase order proven by
// `sweep_run_internal` (main/core/commands/cmd_scan.c), but loops forever and
// stops one phase's radio use before starting the next.
#include "managers/scan_scheduler.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"        // heap_caps_check_integrity_all (diagnostic)
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/system_manager.h"   // xTaskCreate_psram
#include "core/callbacks.h"        // start/stop_pineap_detection
#include "managers/wifi_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/aerial_detector_manager.h"
#include "scans/wifi/ap_scan.h"    // ap_scan_start_async (feeds ap_scan_get_count)

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"          // ble_stop
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/device_detect_scan.h"
#endif

static const char *TAG = "scan_sched";

// Per-phase dwell. Full cycle ~= (phases * dwell); tuned for a responsive but
// stable dashboard rather than exhaustive capture.
#define SCAN_WIFI_DWELL_MS   3000
#define SCAN_PHASE_DWELL_MS  2500
#define SCAN_GAP_MS          300

static volatile bool s_run = false;
static TaskHandle_t s_task = NULL;

// Cooperative stop check between phases.
#define STOP_OR_BREAK() do { if (!s_run) goto done; } while (0)
#define PHASE_DELAY()   vTaskDelay(pdMS_TO_TICKS(SCAN_PHASE_DWELL_MS))
#define GAP_DELAY()     vTaskDelay(pdMS_TO_TICKS(SCAN_GAP_MS))

// DIAGNOSTIC (temporary): after each phase, walk the whole heap. The first
// phase after which this logs "HEAP CORRUPT" is the corruptor. Paired with
// CONFIG_HEAP_POISONING_COMPREHENSIVE it catches overflows near their source.
#define HEAPCHK(name) do { \
    if (!heap_caps_check_integrity_all(true)) { \
        ESP_LOGE(TAG, "HEAP CORRUPT after phase: %s", (name)); \
    } else { \
        ESP_LOGW(TAG, "heap ok after: %s", (name)); \
    } \
} while (0)

static void scan_scheduler_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "scan scheduler started");

    // Aerial and flock create their mutexes in _init() (normally called by the
    // aerial/flock command view, which the dashboard bypasses). Init them once
    // here so their scan callbacks never take a NULL mutex — otherwise the
    // device asserts with "xQueueSemaphoreTake ... pxQueue" the first time a
    // packet arrives during those phases. The mutexes persist across start/stop;
    // only _deinit() frees them, which we deliberately never call.
    aerial_detector_init();
    flock_detector_init();

    // The aerial "network" heuristic flags ordinary WiFi beacons as drone
    // control links — a huge false-positive rate anywhere with normal APs
    // (it filled the Drones tile with phantom hits). Restrict headless
    // detection to precise signatures: OpenDroneID + DJI.
    aerial_detector_enable_network_detection(false);

    while (s_run) {
        // ---- WiFi phases (shared WiFi radio, one owner at a time) ----
        // AP scan via the ap_scan module so ap_scan_get_count() is populated
        // (wifi_manager_start_scan_with_time bypasses that module).
        STOP_OR_BREAK();
        if (ap_scan_start_async() == ESP_OK) {
            int waited = 0;
            while (s_run && ap_scan_is_running() && waited < SCAN_WIFI_DWELL_MS) {
                vTaskDelay(pdMS_TO_TICKS(200));
                waited += 200;
            }
            ap_scan_finish_async();
        }
        GAP_DELAY();
        HEAPCHK("apscan");

        STOP_OR_BREAK();
        wifi_manager_start_station_scan();
        PHASE_DELAY();
        wifi_manager_stop_monitor_mode();
        GAP_DELAY();
        HEAPCHK("station");

        STOP_OR_BREAK();
        start_pineap_detection();
        PHASE_DELAY();
        stop_pineap_detection();
        GAP_DELAY();
        HEAPCHK("pineap");

        STOP_OR_BREAK();
        (void)aerial_detector_start_scan(SCAN_PHASE_DWELL_MS);
        PHASE_DELAY();
        (void)aerial_detector_stop_scan();
        GAP_DELAY();
        HEAPCHK("aerial");

        STOP_OR_BREAK();
        (void)flock_detector_start();
        PHASE_DELAY();
        (void)flock_detector_stop();
        GAP_DELAY();
        HEAPCHK("flock");

#ifndef CONFIG_IDF_TARGET_ESP32S2
        // ---- BLE phases (shared BLE radio) ----
        STOP_OR_BREAK();
        flipper_scan_start();
        PHASE_DELAY();
        flipper_scan_stop();
        GAP_DELAY();
        HEAPCHK("flipper");

        STOP_OR_BREAK();
        airtag_scan_start();
        PHASE_DELAY();
        airtag_scan_stop();
        GAP_DELAY();
        HEAPCHK("airtag");

        STOP_OR_BREAK();
        ble_device_detect_start();
        PHASE_DELAY();
        ble_device_detect_stop();
        GAP_DELAY();
        HEAPCHK("ble_detect");
#endif
    }

done:
    // Leave every radio idle before exiting.
    wifi_manager_stop_monitor_mode();
    stop_pineap_detection();
    (void)aerial_detector_stop_scan();
    (void)flock_detector_stop();
#ifndef CONFIG_IDF_TARGET_ESP32S2
    flipper_scan_stop();
    airtag_scan_stop();
    ble_device_detect_stop();
    ble_stop();
#endif
    ESP_LOGI(TAG, "scan scheduler stopped");
    s_task = NULL;
    vTaskDelete(NULL);
}

void scan_scheduler_start(void) {
    if (s_task != NULL || s_run) {
        return;
    }
    s_run = true;
    BaseType_t ok = xTaskCreate_psram(scan_scheduler_task, "scan_sched", 8192,
                                      NULL, 5, &s_task);
    if (ok != pdPASS) {
        s_run = false;
        s_task = NULL;
        ESP_LOGE(TAG, "failed to create scheduler task");
    }
}

void scan_scheduler_stop(void) {
    s_run = false;  // task exits at its next STOP_OR_BREAK and self-deletes
}

bool scan_scheduler_is_running(void) {
    return s_run;
}
