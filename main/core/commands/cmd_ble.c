// cmd_ble.c
// BLE, AirTag, Flipper, and GATT commands.

#include "core/commands.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/ouis.h"
#include "core/esp_comm_manager.h"
#include "managers/gps_manager.h"
#include "managers/status_display_manager.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#include "scans/ble/advertiser_scan.h"
#include "scans/ble/flipper_scan.h"
#include "host/ble_gap.h"
#endif

#ifndef CONFIG_IDF_TARGET_ESP32S2
void handle_ble_scan_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "-f") == 0) {
        glog("Starting Find the Flippers.\n");
        flipper_scan_start();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-ds") == 0) {
        glog("Starting BLE Spam Detector.\n");
        ble_start_blespam_detector();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        glog("Starting AirTag Scanner.\n");
        ble_start_airtag_scanner();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-r") == 0) {
        glog("Scanning for Raw Packets\n");
        ble_start_raw_ble_packetscan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-g") == 0) {
        glog("Starting GATT Device Scan.\n");
        ble_start_gatt_scan();
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-adv") == 0) {
        glog("Starting BLE Advertiser Scan.\n");
        advertiser_scan_start();
        return;
    }

    if (argc > 2 && strcmp(argv[1], "-oui") == 0) {
        uint8_t oui[3];
        if (!ouis_parse_prefix(argv[2], oui)) {
            glog("Invalid OUI prefix. Use 6 hex digits, e.g. 00:1A:2B.\n");
            return;
        }
        glog("Starting BLE OUI scan for %02X:%02X:%02X.\n", oui[0], oui[1], oui[2]);
        advertiser_scan_start_oui_prefix(oui);
        return;
    }

    if (argc > 2 && strcmp(argv[1], "-vendor") == 0) {
        glog("Starting BLE OUI vendor scan for %s.\n", argv[2]);
        advertiser_scan_start_vendor(argv[2]);
        return;
    }

    if (argc > 1 && strcmp(argv[1], "-s") == 0) {
        glog("Stopping BLE Scan.\n");
        bool advertiser_active = advertiser_scan_is_active();
        advertiser_scan_stop();
        if (!advertiser_active) {
            ble_stop();
        }
        ble_stop_gatt_scan();
        return;
    }

    glog("Invalid Command Syntax.\n");
}

void handle_ble_wardriving(int argc, char **argv) {
    bool stop_flag = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-s") == 0) {
            stop_flag = true;
            break;
        }
    }

    if (stop_flag) {
        ble_stop();
        if (csv_buffer_has_pending_data()) { // Only flush if there's data in buffer
            csv_flush_buffer_to_file();
        }
        csv_file_close();
        gps_manager_deinit(&g_gpsManager);
        gps_manager_set_peer_gps_preferred(false);
        gps_manager_clear_peer_fix();
        printf("BLE wardriving stopped.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving stopped.\n");
        status_display_show_status("BLE Drive Off");
    } else {
        if (csv_file_is_open()) {
            printf("A wardriving CSV session is already active.\n");
            return;
        }
        bool peer_connected = esp_comm_manager_is_connected();
        gps_manager_set_peer_gps_preferred(peer_connected);
        if (!peer_connected) {
            gps_manager_clear_peer_fix();
        }
        if (!peer_connected && !g_gpsManager.isinitilized) {
            gps_manager_init(&g_gpsManager);
        } else if (peer_connected && g_gpsManager.isinitilized) {
            gps_manager_deinit(&g_gpsManager);
        }

        // Open CSV file for BLE wardriving
        esp_err_t err = csv_file_open("ble_wardriving");
        if (err != ESP_OK) {
            printf("Failed to open CSV file for BLE wardriving\n");
            status_display_show_status("CSV Open Fail");
            return;
        }

        ble_start_scanning();
        ble_register_handler(ble_wardriving_callback);
        printf("BLE wardriving started.\n");
        TERMINAL_VIEW_ADD_TEXT("BLE wardriving started.\n");
        if (peer_connected) {
            printf("BLE wardriving GPS source: peer stream preferred.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: peer stream preferred.\n");
        } else {
            printf("BLE wardriving GPS source: local parser.\n");
            TERMINAL_VIEW_ADD_TEXT("BLE wardriving GPS source: local parser.\n");
        }
        status_display_show_status("BLE Drive On");
    }
}

void handle_list_airtags_cmd(int argc, char **argv) {
    ble_list_airtags();
    status_display_show_status("List AirTags");
}

void handle_select_airtag(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectairtag <number>\n");
        status_display_show_status("AirTag Usage");
        return;
    }

    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_airtag(num);
        status_display_show_status("AirTag Select");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("AirTag Invalid");
    }
}

void handle_list_flippers_cmd(int argc, char **argv) {
    flipper_scan_print_results();
    status_display_show_status("List Flipper");
}

void handle_select_flipper_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectflipper <index>\n");
        status_display_show_status("Flipper Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        flipper_scan_select(num);
        status_display_show_status("Flipper Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("Flipper Bad");
    }
}

void handle_list_gatt_cmd(int argc, char **argv) {
    ble_list_gatt_devices();
    status_display_show_status("List GATT");
}

void handle_select_gatt_cmd(int argc, char **argv) {
    if (argc != 2) {
        glog("Usage: selectgatt <index>\n");
        status_display_show_status("GATT Usage");
        return;
    }
    char *endptr;
    int num = (int)strtol(argv[1], &endptr, 10);
    if (*endptr == '\0') {
        ble_select_gatt_device(num);
        status_display_show_status("GATT Pick");
    } else {
        glog("Error: '%s' is not a valid number.\n", argv[1]);
        status_display_show_status("GATT Bad");
    }
}

void handle_enum_gatt_cmd(int argc, char **argv) {
    ble_enumerate_gatt_services();
    status_display_show_status("GATT Enum");
}

void handle_track_gatt_cmd(int argc, char **argv) {
    ble_track_gatt_device();
}

void handle_list_advertisers_cmd(int argc, char **argv) {
    (void)argc;
    (void)argv;
    advertiser_scan_print_devices();
    status_display_show_status("List BLE Adv");
}

#endif
