// cmd_capture.c
// Packet capture, PCAP export, and Wireshark streaming commands.

#include "core/callbacks.h"
#include "core/commands.h"
#include "core/glog.h"
#include "managers/ble_manager.h"
#include "managers/ghostscript_runtime.h"
#include "managers/sd_card_manager.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "managers/zigbee_manager.h"
#include "sdkconfig.h"
#include "vendor/pcap.h"
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/network_constants.h"

static void capture_resolve_pcap_path(const char *arg, char *out, size_t out_len) {
    if (!arg || !out || out_len == 0) return;
    if (arg[0] == '/' || strchr(arg, '/')) {
        snprintf(out, out_len, "%s", arg);
    } else {
        snprintf(out, out_len, "/mnt/ghostesp/pcaps/%s", arg);
    }
}

static int capture_list_dir(const char *dir_path) {
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len < 6 || strcmp(entry->d_name + len - 5, ".pcap") != 0) continue;
        char name[MAX_FILE_NAME_LENGTH - 21];
        strncpy(name, entry->d_name, sizeof(name) - 1);
        name[sizeof(name) - 1] = '\0';
        char path[MAX_FILE_NAME_LENGTH];
        snprintf(path, sizeof(path), "%s/%s", dir_path, name);
        glog("  [%s] %s\n", pcap_has_hc22000_material(path) ? "+" : "-", name);
        count++;
    }
    closedir(dir);
    return count;
}

static void handle_capture_list(void) {
    bool jit_mounted = false;
    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (!sd_card_manager.is_initialized) {
            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                jit_mounted = true;
            }
        }
    }
#endif
    glog("On-device captures:\n");
    int count = capture_list_dir("/mnt/ghostesp/pcaps");
    if (count == 0) glog("  No .pcap files found.\n");
    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
}

static void handle_capture_export(const char *arg) {
    char in_path[MAX_FILE_NAME_LENGTH];
    char out_path[MAX_FILE_NAME_LENGTH];
    int pmkid = 0;
    int handshakes = 0;

    bool jit_mounted = false;
    bool display_suspended = false;
#ifdef CONFIG_BUILD_CONFIG_TEMPLATE
    if (strcmp(CONFIG_BUILD_CONFIG_TEMPLATE, "somethingsomething") == 0) {
        if (!sd_card_manager.is_initialized) {
            if (sd_card_mount_for_flush(&display_suspended) == ESP_OK) {
                jit_mounted = true;
            }
        }
    }
#endif

    capture_resolve_pcap_path(arg, in_path, sizeof(in_path));
    esp_err_t err = pcap_export_hc22000(in_path, out_path, sizeof(out_path), &pmkid, &handshakes);
    if (err == ESP_ERR_NOT_FOUND) {
        glog("No PMKID or M2/M3 handshakes found in %s\n", in_path);
        status_display_show_status("No Handshake");
        if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
        return;
    }
    if (err != ESP_OK) {
        glog("hc22000 export failed for %s (err=%d)\n", in_path, err);
        status_display_show_status("Export Fail");
        if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
        return;
    }
    glog("Exported %s\nPMKID: %d  M2/M3: %d\n", out_path, pmkid, handshakes);
    status_display_show_status("Export hc22000");
    char pk_payload[48];
    snprintf(pk_payload, sizeof(pk_payload), "%d|%d", pmkid, handshakes);
    ghostscript_emit_event("pmkid_exported", pk_payload);
    if (jit_mounted) sd_card_unmount_after_flush(display_suspended);
}

void handle_capture_scan(int argc, char **argv) {
    if (argc < 2 || argc > 5) {
        glog("Error: Incorrect number of arguments.\n");
        status_display_show_status("Capture Usage");
        return;
    }

    char *capturetype = argv[1];

    if (capturetype == NULL || capturetype[0] == '\0') {
        glog("Error: Capture Type cannot be empty.\n");
        status_display_show_status("Capture Empty");
        return;
    }

    // Parse optional "-channel <n>" or "-c <n>" before mode-specific logic.
    // The mode remains argv[1], so existing usage like `capture -wireshark -c 6`
    // and new usage like `capture -probe -channel 6` both work.
    uint8_t fixed_channel = 0;
    bool fixed_channel_set = false;
    int parsed_fixed_channel = 0;
    for (int i = 2; i + 1 < argc; i++) {
        if (strcmp(argv[i], "-channel") == 0 || strcmp(argv[i], "-c") == 0) {
            parsed_fixed_channel = atoi(argv[i + 1]);
            fixed_channel_set = true;
            break;
        }
    }

    bool wifi_channel_lock_mode = strcmp(capturetype, "-wireshark") == 0;

    bool channel_flag_applicable = wifi_channel_lock_mode
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        || strcmp(capturetype, "-802154") == 0
#endif
        ;

    bool channel_flag_ignored_by_mode =
        strcmp(capturetype, "-list") == 0 || strcmp(capturetype, "-export") == 0;

    if (fixed_channel_set && wifi_channel_lock_mode) {
        if (parsed_fixed_channel < 1 || parsed_fixed_channel > MAX_WIFI_CHANNEL) {
            glog("Error: Invalid channel %d. Must be between 1 and %d\n",
                 parsed_fixed_channel, MAX_WIFI_CHANNEL);
            status_display_show_status("Invalid Channel");
            return;
        }
        fixed_channel = (uint8_t)parsed_fixed_channel;
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    } else if (fixed_channel_set && strcmp(capturetype, "-802154") == 0) {
        if (parsed_fixed_channel < 11 || parsed_fixed_channel > 26) {
            glog("Error: Invalid 802.15.4 channel %d. Must be between 11 and 26\n",
                 parsed_fixed_channel);
            status_display_show_status("Invalid Channel");
            return;
        }
        fixed_channel = (uint8_t)parsed_fixed_channel;
#endif
    } else if (fixed_channel_set && !channel_flag_applicable && !channel_flag_ignored_by_mode) {
        glog("Note: -channel ignored for '%s' (not supported for this mode)\n",
             capturetype);
    }

    if (strcmp(capturetype, "-list") == 0) {
        if (argc != 2) {
            if (fixed_channel_set) {
                glog("Note: -channel ignored for '-list'\n");
            } else {
                glog("Usage: capture -list\n");
                return;
            }
        }
        handle_capture_list();
        return;
    }

    if (strcmp(capturetype, "-export") == 0) {
        if (argc != 3) {
            if (fixed_channel_set && argc == 5) {
                glog("Note: -channel ignored for '-export'\n");
            } else {
                glog("Usage: capture -export <pcap-file>\n");
                return;
            }
        }
        handle_capture_export(argv[2]);
        return;
    }

#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
    if (strcmp(capturetype, "-802154") == 0) {
        glog("Starting IEEE 802.15.4 packet capture...\n");
        int err = pcap_file_open("802154", PCAP_CAPTURE_IEEE802154);
        if (err != ESP_OK) {
            glog("Warning: PCAP failed to open (will stream to UART)\n");
            status_display_show_status("PCAP Warn");
        }
        uint8_t ch = 0; // 0 means hopping by default
        if (fixed_channel_set) {
            // Re-validate against the 802.15.4 range 11..26.
            if (fixed_channel < 11 || fixed_channel > 26) {
                glog("Error: Invalid 802.15.4 channel %d. Must be between 11 and 26\n",
                     fixed_channel);
                status_display_show_status("Invalid Channel");
                return;
            }
            ch = fixed_channel;
        } else if (argc == 3 && argv[2]) {
            // Backward-compatible positional "ch<n>" form.
            const char *arg = argv[2];
            if (strncmp(arg, "ch", 2) == 0) arg += 2;
            int parsed = atoi(arg);
            if (parsed >= 11 && parsed <= 26) ch = (uint8_t)parsed; // fixed channel
        }
        zigbee_manager_start_capture(ch);
        status_display_show_status("Capture 802154");
    }
#endif

    if (strcmp(capturetype, "-wireshark") == 0) {
        status_display_show_status("Wireshark WiFi");
        int err = pcap_wireshark_start(PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            status_display_show_status("Wireshark Err");
            return;
        }
        wifi_manager_start_monitor_mode(wifi_raw_scan_callback);

        if (fixed_channel_set) {
            err = wifi_manager_set_wireshark_fixed_channel(fixed_channel);
            if (err != ESP_OK) {
                glog("Error: Failed to set fixed channel %d\n", fixed_channel);
                status_display_show_status("Channel Err");
                pcap_wireshark_stop();
                wifi_manager_stop_monitor_mode();
                return;
            }
            glog("Wireshark capture locked to channel %d\n", fixed_channel);
        } else {
            wifi_manager_start_wireshark_channel_hop();
        }
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-wiresharkble") == 0) {
        status_display_show_status("Wireshark BLE");
        int err = pcap_wireshark_start(PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            status_display_show_status("Wireshark Err");
            return;
        }
        ble_start_capture_wireshark();
    }
#endif

    if (strcmp(capturetype, "-stop") == 0) {
        glog("Stopping packet capture...\n");
        wifi_manager_stop_wireshark_channel_hop();
        wifi_manager_stop_monitor_mode();
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_stop();
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        zigbee_manager_stop_capture();
#endif
        pcap_file_close();
        pcap_wireshark_stop();
        status_display_show_status("Capture Stop");
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(capturetype, "-ble") == 0) {
        printf("Starting BLE packet capture...\n");
        TERMINAL_VIEW_ADD_TEXT("Starting BLE packet capture...\n");
        ble_start_capture();
        status_display_show_status("Capture BLE");
    }

    if (strcmp(capturetype, "-skimmer") == 0) {
        printf("Skimmer detection started.\n");
        TERMINAL_VIEW_ADD_TEXT("Skimmer detection started.\n");
        int err = pcap_file_open("skimmer_scan", PCAP_CAPTURE_BLUETOOTH);
        if (err != ESP_OK) {
            printf("Warning: PCAP capture failed to start\n");
            TERMINAL_VIEW_ADD_TEXT("Warning: PCAP capture failed to start\n");
            status_display_show_status("PCAP Warn");
        } else {
            printf("PCAP capture started\nMonitoring devices\n");
            TERMINAL_VIEW_ADD_TEXT("PCAP capture started\nMonitoring devices\n");
            status_display_show_status("Capture Skimmer");
        }
        // Start skimmer detection
        ble_start_skimmer_detection();

    }
    #endif

    if (strcmp(capturetype, "-list") != 0 &&
        strcmp(capturetype, "-export") != 0 &&
        strcmp(capturetype, "-wireshark") != 0 &&
        strcmp(capturetype, "-stop") != 0
#ifndef CONFIG_IDF_TARGET_ESP32S2
        && strcmp(capturetype, "-wiresharkble") != 0
        && strcmp(capturetype, "-ble") != 0
        && strcmp(capturetype, "-skimmer") != 0
#endif
#if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        && strcmp(capturetype, "-802154") != 0
#endif
    ) {
        glog("Error: Unknown capture type '%s'.\n", capturetype);
        status_display_show_status("Capture Unknown");
    }
}

void handle_capture(int argc, char **argv) {
    if (argc < 2) {
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("Usage: capture [-wireshark|-wiresharkble|-ble|-skimmer|-802154|-list|-export|-stop] [-channel <n>|-c <n>]\n");
        #else
        glog("Usage: capture [-wireshark|-wiresharkble|-ble|-skimmer|-list|-export|-stop] [-channel <n>|-c <n>]\n");
        #endif
        status_display_show_status("Capture Usage");
        return;
    }
#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(argv[1], "-ble") == 0) {
        glog("Starting BLE packet capture...\n");
        ble_start_capture();
        status_display_show_status("Capture BLE");
    }
#endif
}
