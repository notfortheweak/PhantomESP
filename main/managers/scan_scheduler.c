// scan_scheduler.c — continuous multi-radio detection loop. One WiFi + one BLE
// radio, so categories run sequentially in short phases. BLE runs first (faster
// to yield results), then WiFi and the WiFi-promiscuous detectors. After each
// phase's window it feeds the session accumulator (scan_report). When a category
// is "focused" (user drilled into it) the loop runs only that category so it
// refreshes rapidly.
#include "managers/scan_scheduler.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "core/system_manager.h"   // xTaskCreate_psram
#include "core/callbacks.h"        // start/stop_pineap_detection
#include "gui/scan_report.h"       // scan_report_accumulate + SCAT_*
#include "managers/display_manager.h" // display_manager_signal_ble_detection
#include "managers/wifi_manager.h"
#include "managers/flock_detector_manager.h"
#include "managers/aerial_detector_manager.h"
#include "scans/wifi/ap_scan.h"

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"          // ble_stop
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/device_detect_scan.h"
#endif

static const char *TAG = "scan_sched";

#define SCAN_WIFI_DWELL_MS   3000
#define SCAN_PHASE_DWELL_MS  2500
#define SCAN_GAP_MS          300

static volatile bool s_run = false;
static volatile int  s_focus = -1;   // scan_category_id_t, or -1 for round-robin
static TaskHandle_t  s_task = NULL;

#define PHASE_DELAY()   vTaskDelay(pdMS_TO_TICKS(SCAN_PHASE_DWELL_MS))
#define GAP_DELAY()     vTaskDelay(pdMS_TO_TICKS(SCAN_GAP_MS))

#ifndef CONFIG_IDF_TARGET_ESP32S2
// Accumulate a BLE-family category and blink the Bluetooth icon blue whenever a
// new device is added to the session log (a flipper/airtag/BLE device detected
// and logged). total_count is monotonic per session, so a rise == a new device.
static void accumulate_ble(scan_category_id_t cat) {
    int before = scan_report_total_count(cat);
    scan_report_accumulate(cat);
    if (scan_report_total_count(cat) > before) {
        display_manager_signal_ble_detection();
    }
}
#endif

// Run one category's scan window, then snapshot it into the session accumulator.
static void run_phase(scan_category_id_t cat) {
    if (!s_run) return;
    switch (cat) {
    case SCAT_WIFI: {
        // AP scan via the ap_scan module (feeds ap_scan_get_count / results).
        if (ap_scan_start_async() == ESP_OK) {
            int waited = 0;
            while (s_run && ap_scan_is_running() && waited < SCAN_WIFI_DWELL_MS) {
                vTaskDelay(pdMS_TO_TICKS(200));
                waited += 200;
            }
            ap_scan_finish_async();
        }
        GAP_DELAY();
        if (!s_run) return;
        // Station scan (APs stay resident, so accumulate captures both).
        wifi_manager_start_station_scan();
        PHASE_DELAY();
        scan_report_accumulate(SCAT_WIFI);
        wifi_manager_stop_monitor_mode();
        GAP_DELAY();
        break;
    }
    case SCAT_PINEAP:
        start_pineap_detection();
        PHASE_DELAY();
        scan_report_accumulate(SCAT_PINEAP);
        stop_pineap_detection();
        GAP_DELAY();
        break;
    case SCAT_DRONES:
        (void)aerial_detector_start_scan(SCAN_PHASE_DWELL_MS);
        PHASE_DELAY();
        scan_report_accumulate(SCAT_DRONES);
        (void)aerial_detector_stop_scan();
        GAP_DELAY();
        break;
    case SCAT_FLOCK:
        (void)flock_detector_start();
        PHASE_DELAY();
        scan_report_accumulate(SCAT_FLOCK);
        (void)flock_detector_stop();
        GAP_DELAY();
        break;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    case SCAT_FLIPPERS:
        flipper_scan_start();
        PHASE_DELAY();
        accumulate_ble(SCAT_FLIPPERS);
        flipper_scan_stop();
        GAP_DELAY();
        break;
    case SCAT_AIRTAGS:
        airtag_scan_start();
        PHASE_DELAY();
        accumulate_ble(SCAT_AIRTAGS);
        airtag_scan_stop();
        GAP_DELAY();
        break;
    case SCAT_BLE:
        ble_device_detect_start();
        PHASE_DELAY();
        accumulate_ble(SCAT_BLE);
        ble_device_detect_stop();
        GAP_DELAY();
        break;
#endif
    default:
        break;
    }
}

static void scan_scheduler_task(void *arg) {
    (void)arg;
    ESP_LOGI(TAG, "scan scheduler started");

    // Aerial/flock create their mutexes in _init() (normally called by their
    // command view, which the dashboard bypasses). Init once so their scan
    // callbacks never take a NULL mutex.
    aerial_detector_init();
    flock_detector_init();
    // The aerial "network" heuristic flags ordinary beacons as drones; restrict
    // headless detection to precise signatures (OpenDroneID + DJI).
    aerial_detector_enable_network_detection(false);

    while (s_run) {
        int focus = s_focus;
        if (focus >= 0 && focus < SCAT_COUNT) {
            run_phase((scan_category_id_t)focus);
        } else {
            // WiFi-first rotation (proven stable). Running a WiFi phase directly
            // after a BLE phase crashes esp_netif teardown, because BLE start
            // calls esp_wifi_deinit() for coexistence; keeping WiFi first and BLE
            // last avoids that. Fast BLE-on-demand comes from focus mode instead.
            run_phase(SCAT_WIFI);
            run_phase(SCAT_PINEAP);
            run_phase(SCAT_DRONES);
            run_phase(SCAT_FLOCK);
#ifndef CONFIG_IDF_TARGET_ESP32S2
            run_phase(SCAT_FLIPPERS);
            run_phase(SCAT_AIRTAGS);
            run_phase(SCAT_BLE);
#endif
        }
    }

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
    if (s_task != NULL || s_run) return;
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
    s_run = false;   // task finishes its phase, stops radios, self-deletes
}

bool scan_scheduler_is_running(void) {
    return s_run;
}

void scan_scheduler_set_focus(int category) {
    s_focus = category;
}
