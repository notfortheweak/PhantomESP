// cmd_help.c
// Interactive help command.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Help command
void handle_help(int argc, char **argv) {
    const char *category = (argc > 1) ? argv[1] : "unknown"; // Default to "unknown" if no category is provided to fall through ifs

    // List of all categories to print in order. Detection leads. sweep,
    // flockscan, pineap, aerialscan and wpa3check are broken out of
    // "detect" into their own top-level topics (same layer as
    // detect/wifi/ble) so their usage fits on-screen without scrolling
    // past it -- see 'help detect' for the rest of the detection family
    // (airtags/flippers/blescan).
    const char *all_categories[] = {
        "detect", "sweep", "flockscan", "pineap", "aerialscan", "wpa3check", "wifi", "ble", "gps", "wigle", "sd"
#ifdef CONFIG_HAS_INFRARED
        , "ir"
#endif
    };
    int num_categories = sizeof(all_categories) / sizeof(all_categories[0]);

    if (strcmp(category, "all") == 0) {
        for (int i = 0; i < num_categories; ++i) {
            // Recursively call this function for each category
            char *fake_argv[] = { "help", (char *)all_categories[i] };
            handle_help(2, fake_argv);
        }
        return;
    }

    if (strcmp(category, "sweep") == 0) {
        glog("\nsweep\n");
        glog("    Description: Full environment sweep - scans WiFi APs, stations and\n");
        glog("                 BLE devices, then saves a comprehensive report to SD card.\n");
        glog("    Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
        glog("    Arguments:\n");
        glog("        -w  : WiFi scan duration per phase in seconds (default: 5)\n");
        glog("        -b  : BLE scan duration per phase in seconds (default: 5)\n");
        glog("    Output: /mnt/ghostesp/sweeps/sweep_N.csv\n\n");
        glog("See also: help detect, help flockscan, help pineap.\n\n");
        return;
    }

    if (strcmp(category, "flockscan") == 0) {
        glog("\nflockscan\n");
        glog("    Description: Start Flock Safety surveillance-camera detection.\n");
        glog("    Usage: flockscan\n\n");
        glog("flocklist\n");
        glog("    Description: List detected surveillance devices (MAC, method,\n");
        glog("                 confidence, signal, channel, hits, SSID).\n");
        glog("    Usage: flocklist\n\n");
        glog("flockstop\n");
        glog("    Description: Stop Flock camera detection.\n");
        glog("    Usage: flockstop\n\n");
        glog("See also: help detect, help sweep, help pineap.\n\n");
        return;
    }

    if (strcmp(category, "pineap") == 0) {
        glog("\npineap\n");
        glog("    Description: Detect WiFi Pineapple / KARMA-style rogue access points.\n");
        glog("    Usage: pineap [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop PineAP detection\n\n");
        glog("See also: help detect, help sweep, help flockscan.\n\n");
        return;
    }

    if (strcmp(category, "aerialscan") == 0) {
        glog("\naerialscan\n");
        glog("    Description: Detect drones broadcasting OpenDroneID / Remote ID\n");
        glog("                 (Phase 1: WiFi | Phase 2: BLE).\n");
        glog("    Usage: aerialscan [seconds]\n\n");
        glog("aeriallist\n");
        glog("    Description: List detected drones with ID, type, location, altitude,\n");
        glog("                 speed, and operator position when broadcast.\n");
        glog("    Usage: aeriallist\n\n");
        glog("aerialtrack\n");
        glog("    Description: Track a specific detected drone by index or MAC.\n");
        glog("    Usage: aerialtrack <device_index|mac_address>\n");
        glog("    Note: run 'aeriallist' first to see available devices.\n\n");
        glog("aerialstop\n");
        glog("    Description: Stop drone detection.\n");
        glog("    Usage: aerialstop\n\n");
        glog("See also: help detect, help sweep, help flockscan, help pineap.\n\n");
        return;
    }

    if (strcmp(category, "wpa3check") == 0) {
        glog("\nwpa3check\n");
        glog("    Description: Audit AP security posture - WPA3 presence, transition\n");
        glog("                 mode, and PMF (management-frame protection).\n");
        glog("    Usage: wpa3check (after 'scanap', optionally 'select -a <index>')\n\n");
        glog("See also: help detect, help sweep, help flockscan, help pineap, help aerialscan.\n\n");
        return;
    }

    if (strcmp(category, "detect") == 0) {
        glog("\nCounter-Surveillance Detection:\n\n");
        glog("sweep      - Full environment sweep, WiFi + BLE  (help sweep)\n");
        glog("flockscan  - Flock Safety camera detection       (help flockscan)\n");
        glog("pineap     - WiFi Pineapple / rogue AP detection (help pineap)\n");
        glog("aerialscan - Drone / Remote ID detection         (help aerialscan)\n");
        glog("wpa3check  - AP security audit (WPA3 / PMF)      (help wpa3check)\n\n");
#ifndef CONFIG_IDF_TARGET_ESP32S2
        glog("listairtags\n");
        glog("    Description: List nearby Apple AirTags / trackers.\n");
        glog("    Usage: listairtags   (populated by 'blescan -a')\n\n");
        glog("selectairtag\n");
        glog("    Description: Select a discovered AirTag by index to inspect/track.\n");
        glog("    Usage: selectairtag <index>\n\n");
        glog("listflippers\n");
        glog("    Description: List nearby Flipper Zero devices.\n");
        glog("    Usage: listflippers   (populated by 'blescan -f')\n\n");
        glog("selectflipper\n");
        glog("    Description: Select a discovered Flipper by index.\n");
        glog("    Usage: selectflipper <index>\n\n");
        glog("blescan -ds\n");
        glog("    Description: Detect BLE advertisement-spam / attack floods nearby.\n");
        glog("    Usage: blescan -ds   (blescan -s to stop)\n\n");
#endif
        glog("Related: 'congestion' and 'listenprobes' (help wifi), 'startwd' (help gps).\n\n");
        return;
    }

    if (strcmp(category, "wifi") == 0) {
        glog("\nWi-Fi Commands:\n\n");
        glog("scanap\n");
        glog("    Description: Start a Wi-Fi access point (AP) scan.\n");
        glog("    Usage: scanap [seconds]\n\n");
        glog("scansta\n");
        glog("    Description: Start scanning for Wi-Fi stations (hops channels).\n");
        glog("    Usage: scansta\n\n");
        glog("scanall\n");
        glog("    Description: Perform combined AP and Station scan, display results.\n");
        glog("    Usage: scanall [seconds]\n\n");
        glog("stopscan\n");
        glog("    Description: Stop any ongoing Wi-Fi scan.\n");
        glog("    Usage: stopscan\n\n");
        glog("list\n");
        glog("    Description: List Wi-Fi scan results or connected stations.\n");
        glog("    Usage: list -a | list -s\n");
        glog("    Arguments:\n");
        glog("        -a  : Show access points from Wi-Fi scan\n");
        glog("        -s  : List connected stations\n\n");
        glog("select\n");
        glog("    Description: Select access point(s) or a station by index from the scan results.\n");
        glog("    Usage: select -a <num[,num,...]> | select -s <num>\n");
        glog("    Arguments:\n");
        glog("        -a  : AP selection index (supports multiple: 1,3,5)\n");
        glog("        -s  : Station selection index\n");
        glog("    Examples:\n");
        glog("        select -a 4      : Select single AP at index 4\n");
        glog("        select -a 1,3,5  : Select multiple APs at indices 1, 3, and 5\n\n");
        glog("congestion\n");
        glog("    Description: Display Wi-Fi channel congestion chart.\n");
        glog("    Usage: congestion\n\n");
        glog("listenprobes\n");
        glog("    Description: Listen for and log probe requests (device presence).\n");
        glog("    Usage: listenprobes [channel] [stop]\n");
        glog("    Arguments:\n");
        glog("        [channel] : Listen on specific channel (1-165), omit for channel hopping\n");
        glog("        stop      : Stop probe request listening\n\n");
        glog("trackap\n");
        glog("    Description: Track selected AP signal strength (RSSI).\n");
        glog("    Usage: trackap\n");
        glog("    Note: select an ap first with 'select -a <index>'\n\n");
        glog("tracksta\n");
        glog("    Description: Track selected station signal strength (RSSI).\n");
        glog("    Usage: tracksta\n");
        glog("    Note: select a station first with 'select -s <index>'\n\n");
        glog("wdstream\n");
        glog("    Description: Stream WiFi/BLE observations over serial for companion-app wardriving.\n");
        glog("    Usage: wdstream start [-wifi] [-ble] [-i <ms>] [-ch auto|1|1,6,11]\n");
        glog("           wdstream stop | wdstream status\n\n");
        glog("connect\n");
        glog("    Description: Connect to a specific WiFi network and save credentials.\n");
        glog("    Usage: connect <SSID> [Password]\n\n");
        glog("autoreconnect\n");
        glog("    Description: Toggle WiFi station auto-reconnect after involuntary disconnects.\n");
        glog("    Usage: autoreconnect <on|off>\n\n");
        glog("apcred\n");
        glog("    Description: Change or reset the device AP credentials.\n");
        glog("    Usage: apcred <ssid> <password>\n");
        glog("           apcred -r (reset to defaults)\n\n");
        glog("apenable\n");
        glog("    Description: Enable or disable the Access Point across reboots.\n");
        glog("    Usage: apenable <on|off>\n\n");
#if CONFIG_IDF_TARGET_ESP32C5
        glog("setcountry\n");
        glog("    Description: Set the Wi-Fi country code.\n");
        glog("    Usage: setcountry <CC>\n");
        glog("    Arguments:\n");
        glog("        <CC> : Country code (\"01\" world-safe) or two-letter ISO (e.g., US)\n\n");
#endif
        glog("See also: 'wpa3check' (help wpa3check), 'pineap' (help pineap), 'sweep' (help sweep).\n\n");
        return;
    }

#ifndef CONFIG_IDF_TARGET_ESP32S2
    if (strcmp(category, "ble") == 0) {
        glog("\nBLE Commands:\n\n");
        glog("blescan\n");
        glog("    Description: Handle BLE scanning with various modes.\n");
        glog("    Usage: blescan [OPTION]\n");
        glog("    Arguments:\n");
        glog("        -a   : Start AirTag scanner (see 'listairtags')\n");
        glog("        -f   : Start 'Find the Flippers' mode (see 'listflippers')\n");
        glog("        -ds  : Start BLE spam / attack detector\n");
        glog("        -adv : Start parsed BLE advertiser scan (see 'listadv')\n");
        glog("        -oui <prefix>    : Scan BLE advertisers matching an OUI prefix\n");
        glog("        -vendor <vendor> : Scan BLE advertisers matching an OUI vendor\n");
        glog("        -g   : Start GATT scanner for connectable devices\n");
        glog("        -r   : Scan for raw BLE packets\n");
        glog("        -s   : Stop BLE scanning\n\n");
        glog("listadv\n");
        glog("    Description: List parsed BLE advertisers from blescan -adv.\n");
        glog("    Usage: listadv\n\n");
        glog("blewardriving\n");
        glog("    Description: Start/Stop BLE wardriving with GPS logging.\n");
        glog("    Usage: blewardriving [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop BLE wardriving\n\n");
        glog("See also: 'listairtags', 'listflippers', 'blescan -ds' (help detect).\n\n");
        return;
    }
#endif

    if (strcmp(category, "gps") == 0) {
        glog("\nGPS & Wardriving Commands:\n\n");
        glog("gpsinfo\n    Show GPS info.\n    Usage: gpsinfo [-s]\n\n");
        glog("gpspin\n    Set GPS RX pin for external GPS module.\n    Usage: gpspin <pin>\n\n");
        glog("gpsbaud\n    Set GPS baud rate or auto-detect it.\n    Usage: gpsbaud <auto|0|4800|9600|19200|38400|57600|115200>\n\n");
        glog("startwd\n    Start GPS wardriving.\n    Usage: startwd [-s] [--helper] [--channels <csv>] [--hop <ms>] [--weighted]\n\n");
        return;
    }

    if (strcmp(category, "wigle") == 0) {
        glog("\nWiGLE Commands:\n\n");
        glog("wigle API <encoded|name:token>\n    Set WiGLE API credentials (encoded token or legacy format).\n\n");
        glog("wigle auto on/off\n    Enable/disable auto-upload.\n\n");
        glog("wigle donate on/off\n    Enable/disable WiGLE donate flag.\n\n");
        glog("wigle show\n    Show WiGLE settings.\n\n");
        glog("wigle list\n    Show uploaded CSV memory.\n\n");
        glog("wigle files [page]\n    List CSVs in /mnt/ghostesp/gps/ for manual upload.\n\n");
        glog("wigle upload <filename>\n    Upload a specific CSV file.\n\n");
        glog("wigle upload all\n    Upload all pending queue files.\n\n");
        glog("wigle stats\n    Show account stats for current API key.\n\n");
        return;
    }

    if (strcmp(category, "sd") == 0) {
        glog("\nSD Card Commands:\n\n");
        glog("-- File Operations (machine-parsable) --\n");
        glog("sd status\n    Show SD mount status, type, capacity, usage.\n    Usage: sd status\n\n");
        glog("sd list\n    List files/dirs with indices.\n    Usage: sd list [path]\n\n");
        glog("sd info\n    Show file/dir details.\n    Usage: sd info <index|path>\n\n");
        glog("sd size\n    Get file size.\n    Usage: sd size <index|path>\n\n");
        glog("sd read\n    Read file (chunked downloads).\n    Usage: sd read <index|path> [offset] [length] [--base64]\n\n");
        glog("sd write\n    Create/overwrite file with base64 data.\n    Usage: sd write <path> <base64>\n\n");
        glog("sd append\n    Append base64 data to file.\n    Usage: sd append <path> <base64>\n\n");
        glog("sd mkdir\n    Create directory.\n    Usage: sd mkdir <path>\n\n");
        glog("sd rm\n    Delete file or empty directory.\n    Usage: sd rm <index|path>\n\n");
        glog("sd tree\n    Recursive listing.\n    Usage: sd tree [path] [depth]\n\n");
        glog("-- Pin Configuration --\n");
        glog("sd_config\n    Show current SD GPIO pin configuration.\n    Usage: sd_config\n\n");
        glog("sd_pins_mmc\n    Set GPIO pins for SDMMC mode.\n    Usage: sd_pins_mmc <clk> <cmd> <d0> <d1> <d2> <d3>\n\n");
        glog("sd_pins_spi\n    Set GPIO pins for SPI mode.\n    Usage: sd_pins_spi <cs> <clk> <miso> <mosi>\n\n");
        glog("sd_save_config\n    Save pin config to NVS.\n    Usage: sd_save_config\n\n");
        return;
    }

#ifdef CONFIG_HAS_INFRARED
    if (strcmp(category, "ir") == 0) {
        glog("\nInfrared Commands (receive / learn):\n\n");
        glog("ir rx\n");
        glog("    Description: Receive and display IR signals (Matrix mode).\n");
        glog("    Usage: ir rx [timeout]\n\n");
        glog("ir learn\n");
        glog("    Description: Learn an IR signal and save to file.\n");
        glog("    Usage: ir learn <path>\n\n");
        glog("ir list\n");
        glog("    Description: List IR files in default directory.\n");
        glog("    Usage: ir list [path]\n\n");
        glog("ir show\n");
        glog("    Description: Show content of an IR file.\n");
        glog("    Usage: ir show <path>\n\n");
        return;
    }
#endif

    glog("\nGhost ESP Command Categories:\n\n");

    glog("  help detect     - Counter-surveillance detection (start here)\n");
    glog("  help sweep      - Full environment sweep, WiFi + BLE\n");
    glog("  help flockscan  - Flock Safety camera detection\n");
    glog("  help pineap     - WiFi Pineapple / rogue AP detection\n");
    glog("  help aerialscan - Drone / Remote ID detection\n");
    glog("  help wpa3check  - AP security audit (WPA3 / PMF)\n");
    glog("  help wifi       - Wi-Fi scan & analyze commands\n");
#ifndef CONFIG_IDF_TARGET_ESP32S2
    glog("  help ble        - Bluetooth/BLE scan commands\n");
#endif
    glog("  help gps        - GPS & wardriving commands\n");
    glog("  help wigle      - WiGLE upload commands\n");
    glog("  help sd         - SD card commands\n");
#ifdef CONFIG_HAS_INFRARED
    glog("  help ir         - Infrared receive/learn commands\n");
#endif
    glog("  help all        - All commands\n\n");

    glog("Type 'help <category>' for details on that category.\n\n");
}
