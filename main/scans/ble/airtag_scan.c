/**
 * @file airtag_scan.c
 * @brief Apple AirTag device detection scan implementation
 * 
 * This module handles BLE scanning for Apple AirTag devices including:
 * - Starting and stopping AirTag detection scans
 * - Managing discovered device storage
 * - Listing and selecting discovered AirTags
 * - Spoofing selected AirTag devices
 */

#include "scans/ble/airtag_scan.h"
#include "core/scan_saver.h"
#include "core/glog.h"
#include "core/utils.h"
#include "managers/ble_manager.h"
#include "managers/status_display_manager.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/ble.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Maximum number of AirTags to track based on available memory
#ifdef CONFIG_SPIRAM
#define MAX_AIRTAGS 50
#else
#define MAX_AIRTAGS 16
#endif

// RSSI log interval to avoid spamming logs
#define AIRTAG_RSSI_LOG_INTERVAL_MS 5000

// Module tag for logging
static const char *TAG = "AirTagScan";

// Structure to store discovered AirTag information
typedef struct {
    ble_addr_t addr;
    uint8_t payload[BLE_HS_ADV_MAX_SZ]; // Store the full payload
    size_t payload_len;
    int8_t rssi;
} AirTagDevice;

// Discovered AirTag storage
EXT_RAM_BSS_ATTR static AirTagDevice discovered_airtags[MAX_AIRTAGS];
static int discovered_airtag_count = 0;
static int selected_airtag_index = -1;
static TickType_t airtag_last_rssi_log[MAX_AIRTAGS];
static volatile bool airtag_scan_active = false;

// Forward declarations
static void airtag_scanner_callback(struct ble_gap_event *event, size_t len);

// ============================================================================
// Helper Functions
// ============================================================================

/**
 * @brief Check if payload contains Apple AirTag patterns
 * 
 * @param payload BLE advertisement payload
 * @param len Length of payload
 * @return true if AirTag pattern detected
 */
static bool is_airtag_pattern(const uint8_t *payload, size_t len) {
    if (payload == NULL || len < 4) {
        return false;
    }

    // Walk the BLE advertisement's AD structures ([length][type][data...]) and
    // match ONLY the manufacturer-specific-data field (AD type 0xFF) whose
    // Apple company ID (0x4C 0x00) is followed by the Apple message type 0x12
    // (Find My / Offline Finding). AirTags and third-party Find My trackers
    // broadcast type 0x12; AirPods use 0x07 (proximity pairing), iPhones/other
    // Apple gear use 0x0F/0x10 (nearby) -- all of which now fall through to the
    // general BLE list.
    //
    // Anchoring to the AD field is what actually fixes the AirPods false hit: a
    // raw byte-scan of the whole payload for "4C 00 12" also matches that run
    // when it appears by coincidence *inside* AirPods proximity-pairing data,
    // so open AirPods were still reported as AirTags. Parsing the AD structure
    // checks the company ID + type only where they are structurally meaningful.
    size_t i = 0;
    while (i < len) {
        uint8_t ad_len = payload[i];
        if (ad_len == 0) break;               // end of data / padding
        if (i + 1 + ad_len > len) break;      // truncated AD structure
        uint8_t ad_type = payload[i + 1];
        const uint8_t *ad_data = &payload[i + 2];
        size_t ad_data_len = (size_t)ad_len - 1;
        if (ad_type == 0xFF && ad_data_len >= 3 &&
            ad_data[0] == 0x4C && ad_data[1] == 0x00 && ad_data[2] == 0x12) {
            return true;
        }
        i += 1 + ad_len;                      // advance to next AD structure
    }
    return false;
}

/**
 * @brief Find AirTag device index by MAC address
 * 
 * @param addr BLE address to search for
 * @return int Index of device if found, -1 otherwise
 */
static int find_airtag_by_addr(const ble_addr_t *addr) {
    for (int i = 0; i < discovered_airtag_count; i++) {
        if (memcmp(discovered_airtags[i].addr.val, addr->val, 6) == 0) {
            return i;
        }
    }
    return -1;
}

/**
 * @brief Log AirTag discovery information
 * 
 * @param idx Index of discovered AirTag
 * @param total Total count of discovered AirTags
 * @param mac MAC address string
 * @param rssi RSSI value
 */
static void log_airtag_discovery(int idx, int total, const char *mac, int8_t rssi) {
    glog("[%d] AirTag Found (Total: %d)\n"
         "     MAC: %s,\n"
         "     RSSI: %d dBm (%s),\n",
         idx, total, mac, rssi, rssi_to_proximity(rssi));
}

/**
 * @brief Log AirTag RSSI update
 * 
 * @param idx Index of AirTag
 * @param mac MAC address string
 * @param rssi RSSI value
 */
static void log_airtag_rssi_update(int idx, const char *mac, int8_t rssi) {
    glog("[%d] AirTag RSSI Update: %d dBm (%s)\n"
         "     MAC: %s\n",
         idx, rssi, rssi_to_proximity(rssi), mac);
}

// ============================================================================
// BLE Callback
// ============================================================================

/**
 * @brief BLE callback for AirTag detection during scan
 * 
 * This callback is invoked for each BLE advertisement received during
 * an AirTag scan. It checks if the device is an AirTag and adds
 * it to the discovered devices list.
 * 
 * @param event BLE gap event containing advertisement data
 * @param len Length of event data
 */
static void airtag_scanner_callback(struct ble_gap_event *event, size_t len) {
    (void)len;
    if (!airtag_scan_active || event->type != BLE_GAP_EVENT_DISC) {
        return;
    }
    
    const uint8_t *payload = event->disc.data;
    size_t payload_len = event->disc.length_data;

    if (!is_airtag_pattern(payload, payload_len)) {
        return;
    }

    // Check if this AirTag is already discovered
    int existing_idx = find_airtag_by_addr(&event->disc.addr);
    
    if (existing_idx >= 0) {
        // Update RSSI for existing AirTag
        discovered_airtags[existing_idx].rssi = event->disc.rssi;

        TickType_t now = xTaskGetTickCount();
        TickType_t elapsed = now - airtag_last_rssi_log[existing_idx];
        if (airtag_last_rssi_log[existing_idx] == 0 || 
            elapsed >= pdMS_TO_TICKS(AIRTAG_RSSI_LOG_INTERVAL_MS)) {
            char macAddress[18];
            format_mac_address(discovered_airtags[existing_idx].addr.val, 
                              macAddress, sizeof(macAddress), false);
            log_airtag_rssi_update(existing_idx, macAddress, event->disc.rssi);
            airtag_last_rssi_log[existing_idx] = now;
        }
        return;
    }

    // Add new AirTag to discovered list
    if (discovered_airtag_count >= MAX_AIRTAGS) {
        return;
    }

    AirTagDevice *new_tag = &discovered_airtags[discovered_airtag_count];
    memcpy(new_tag->addr.val, event->disc.addr.val, 6);
    new_tag->addr.type = event->disc.addr.type;
    new_tag->rssi = event->disc.rssi;
    memcpy(new_tag->payload, payload, payload_len);
    new_tag->payload_len = payload_len;
    airtag_last_rssi_log[discovered_airtag_count] = xTaskGetTickCount();

    char macAddress[18];
    format_mac_address(event->disc.addr.val, macAddress, sizeof(macAddress), false);
    log_airtag_discovery(discovered_airtag_count, discovered_airtag_count + 1,
                         macAddress, event->disc.rssi);

    discovered_airtag_count++;
}

// ============================================================================
// Scan Operations - One function at a time
// ============================================================================

/**
 * @brief Start scanning for Apple AirTag devices
 */
void airtag_scan_start(void) {
    // Initialize BLE first if needed (same pattern as flipper_scan)
    if (!ble_is_initialized()) {
        ble_init();
    }

    // Wait for BLE stack to be ready with proper sync
    if (!ble_wait_for_ready()) {
        ESP_LOGE(TAG, "BLE stack not ready for AirTag scanner");
        return;
    }

    // Now clear data structures and set active flag
    memset(discovered_airtags, 0, sizeof(discovered_airtags));
    memset(airtag_last_rssi_log, 0, sizeof(airtag_last_rssi_log));
    discovered_airtag_count = 0;
    selected_airtag_index = -1;
    airtag_scan_active = true;

    ESP_LOGI(TAG, "AirTag scanner: registering handler and starting BLE scan");
    ble_register_handler(airtag_scanner_callback);
    ble_start_scanning();
}

/**
 * @brief Stop scanning for Apple AirTag devices
 */
void airtag_scan_stop(void) {
    // Save discovered AirTags to file if any were found
    if (airtag_scan_active && discovered_airtag_count > 0) {
        scan_file_t sf = SCAN_FILE_INIT;
        if (scan_file_open(&sf, "airtag_scan", "txt") == ESP_OK) {
            scan_file_printf(&sf, "--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
            for (int i = 0; i < discovered_airtag_count; i++) {
                char mac[18];
                format_mac_address(discovered_airtags[i].addr.val, mac, sizeof(mac), false);
                scan_file_printf(&sf, "[%d] MAC: %s, RSSI: %d dBm\n",
                                 i, mac, discovered_airtags[i].rssi);
            }
            scan_file_close(&sf);
        }
    }

    airtag_scan_active = false;
    ble_unregister_handler(airtag_scanner_callback);
}

/**
 * @brief Get the count of discovered AirTag devices
 */
int airtag_scan_get_count(void) {
    return discovered_airtag_count;
}

int airtag_scan_get_device_data(int index, uint8_t *mac, int8_t *rssi) {
    if (index < 0 || index >= discovered_airtag_count) return -1;
    if (mac) memcpy(mac, discovered_airtags[index].addr.val, 6);
    if (rssi) *rssi = discovered_airtags[index].rssi;
    return 0;
}

/**
 * @brief Print the list of discovered AirTag devices
 */
void airtag_scan_print_results(void) {
    glog("--- Discovered AirTags (%d) ---\n", discovered_airtag_count);
    if (discovered_airtag_count == 0) {
        glog("No AirTags discovered yet.\n");
        return;
    }

    scan_file_t sf = SCAN_FILE_INIT;
    bool saving = (scan_file_open(&sf, "airtag_scan", "txt") == ESP_OK);
    if (saving) scan_file_printf(&sf, "--- Discovered AirTags (%d) ---\n", discovered_airtag_count);

    for (int i = 0; i < discovered_airtag_count; i++) {
        char macAddress[18];
        format_mac_address(discovered_airtags[i].addr.val, macAddress, sizeof(macAddress), false);

        glog("[%d] MAC: %s,\n"
             "     RSSI: %d dBm (%s)%s\n",
             i, macAddress, discovered_airtags[i].rssi,
             rssi_to_proximity(discovered_airtags[i].rssi),
             (i == selected_airtag_index) ? " (Selected)" : "");
        if (saving) {
            scan_file_printf(&sf, "[%d] MAC: %s, RSSI: %d dBm\n",
                             i, macAddress, discovered_airtags[i].rssi);
        }
    }
    glog("-------------------------------\n");
    if (saving) scan_file_close(&sf);
}

/**
 * @brief Check if an AirTag scan is currently active
 */
bool airtag_scan_is_active(void) {
    return airtag_scan_active;
}

/**
 * @brief Select an AirTag for spoofing
 */
void airtag_scan_select(int index) {
    if (index < 0 || index >= discovered_airtag_count) {
        glog("Error: Invalid AirTag index %d. Use 'listairtags' to see valid indices.\n", index);
        selected_airtag_index = -1;
        return;
    }

    selected_airtag_index = index;
    char macAddress[18];
    format_mac_address(discovered_airtags[index].addr.val, macAddress, sizeof(macAddress), false);

    glog("Selected AirTag [%d]: MAC %s\n", index, macAddress);
}

