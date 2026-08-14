// cmd_netscan.c
// Network reconnaissance commands: port, ARP, SSH, NetBIOS, HTTP banner, SNMP,
// congestion, and probe listening.

#include "core/commands.h"
#include "core/callbacks.h"
#include "core/glog.h"
#include "core/memory_debug.h"
#include "managers/status_display_manager.h"
#include "managers/wifi_manager.h"
#include "managers/sd_card_manager.h"
#include "vendor/pcap.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/network_constants.h"

static bool normalize_subnet_prefix_arg(const char *arg, char *out, size_t out_len) {
    if (arg == NULL || out == NULL || out_len < 16) {
        return false;
    }

    char tmp[32];
    strlcpy(tmp, arg, sizeof(tmp));
    char *slash = strchr(tmp, '/');
    if (slash != NULL) {
        *slash = '\0';
    }

    unsigned int a = 0, b = 0, c = 0, d = 0;
    int consumed = 0;
    if (sscanf(tmp, "%u.%u.%u.%u%n", &a, &b, &c, &d, &consumed) == 4 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255 || d > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    consumed = 0;
    if (sscanf(tmp, "%u.%u.%u.%n", &a, &b, &c, &consumed) == 3 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    consumed = 0;
    if (sscanf(tmp, "%u.%u.%u%n", &a, &b, &c, &consumed) == 3 && tmp[consumed] == '\0') {
        if (a > 255 || b > 255 || c > 255) return false;
        snprintf(out, out_len, "%u.%u.%u.", a, b, c);
        return true;
    }

    return false;
}


void handle_congestion_cmd(int argc, char **argv) {
    wifi_manager_start_scan();
    status_display_show_status("Congest Scan");

    uint16_t ap_count = 0;
    wifi_ap_record_t *ap_records = NULL;

    wifi_manager_get_scan_results_data(&ap_count, &ap_records);

    if (ap_count == 0 || ap_records == NULL) {
        glog("No APs found during scan.\n");
        status_display_show_status("No AP Found");
        return;
    }

    int unique_count = 0;
    int *channels = spiram_malloc((size_t)ap_count * sizeof(int));
    int *counts = spiram_malloc((size_t)ap_count * sizeof(int));
    if (!channels || !counts) {
        free(channels);
        free(counts);
        glog("Error: Failed to allocate memory for channel counts.\n");
        status_display_show_status("Congest OOM");
        return;
    }
    int max_count = 0;
    for (int i = 0; i < ap_count; i++) {
        int ch = ap_records[i].primary;
        if (ch <= 0) continue;
        int idx = -1;
        for (int j = 0; j < unique_count; j++) {
            if (channels[j] == ch) { idx = j; break; }
        }
        if (idx >= 0) {
            counts[idx]++;
        } else {
            channels[unique_count] = ch;
            counts[unique_count] = 1;
            idx = unique_count++;
        }
        if (counts[idx] > max_count) {
            max_count = counts[idx];
        }
    }
    for (int i = 0; i < unique_count - 1; i++) {
        for (int j = i + 1; j < unique_count; j++) {
            if (channels[i] > channels[j]) {
                int tmp_ch = channels[i]; channels[i] = channels[j]; channels[j] = tmp_ch;
                int tmp_cnt = counts[i]; counts[i] = counts[j]; counts[j] = tmp_cnt;
            }
        }
    }

    glog("\nChannel Congestion:\n\n");
    const char* header = "+----+-------+------------+\n";
    const char* separator = "+----+-------+------------+\n";
    const char* row_format = "| %2d | %5d | %s |\n";
    const char* footer = "+----+-------+------------+\n";

    glog("%s", header);
    glog("| CH | Count | Bar        |\n");
    glog("%s", separator);

    const int max_bar_length = 10;
    char display_bar[max_bar_length * 4]; // Generous buffer: 3 bytes/block + 1 space/pad + null

    for (int i = 0; i < unique_count; i++) {
        int ch = channels[i];
        int cnt = counts[i];
        int bar_length = 0;
        if (max_count > 0) {
            bar_length = (int)(((float)cnt / max_count) * max_bar_length);
            if (bar_length == 0 && cnt > 0) bar_length = 1;
        }
        char *ptr = display_bar;
        for (int j = 0; j < bar_length; ++j) {
            *ptr++ = '#';
        }
        int spaces_needed = max_bar_length - bar_length;
        for (int j = 0; j < spaces_needed; ++j) {
            *ptr++ = ' ';
        }
        *ptr = '\0';
        glog(row_format, ch, cnt, display_bar);
    }
    free(channels);
    free(counts);
    glog("%s", footer);
}

void handle_listen_probes_cmd(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "stop") == 0) {
        wifi_manager_stop_monitor_mode();
        pcap_file_close();
        g_listen_probes_save_to_sd = false;
        glog("Probe request listening stopped.\n");
        status_display_show_status("Probes Stop");
        return;
    }

    uint8_t channel = 0;
    bool channel_hopping = true;

    if (argc > 1) {
        char *endptr;
        long ch = strtol(argv[1], &endptr, 10);
        if (*endptr == '\0' && ch >= 1 && ch <= MAX_WIFI_CHANNEL) {
            channel = (uint8_t)ch;
            channel_hopping = false;
            glog("Starting to listen for probe requests on channel %d...\n", channel);
            char status_msg[18];
            snprintf(status_msg, sizeof(status_msg), "Probes Ch %02d", channel);
            status_display_show_status(status_msg);
        } else {
            glog("Invalid channel: %s. Valid range: 1-%d\n", argv[1], MAX_WIFI_CHANNEL);
            status_display_show_status("Channel Bad");
            return;
        }
    } else {
        glog("Starting to listen for probe requests (channel hopping)...\n");
        status_display_show_status("Probes Hop");
    }

    bool sd_available = sd_card_exists("/mnt/ghostesp/pcaps");
    g_listen_probes_save_to_sd = sd_available;
    if (sd_available) {
        int err = pcap_file_open("probelisten", PCAP_CAPTURE_WIFI);
        if (err != ESP_OK) {
            glog("Warning: PCAP file open failed; probes will not be saved to SD card.\n");
            g_listen_probes_save_to_sd = false;
            status_display_show_status("PCAP Warn");
        }
    } else {
        glog("SD card not available; probe PCAP disabled.\n");
        status_display_show_status("SD Missing");
    }

    if (channel_hopping) {
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    } else {
        esp_wifi_set_channel(channel, WIFI_SECOND_CHAN_NONE);
        wifi_manager_start_monitor_mode(wifi_listen_probes_callback);
    }
}
