// cmd_help.c
// Interactive help command.

#include "core/commands.h"
#include "core/glog.h"
#include "managers/views/terminal_screen.h"
#include "sdkconfig.h"

#include "core/network_constants.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Help command
void handle_help(int argc, char **argv) {
    const char *category = (argc > 1) ? argv[1] : "unknown"; // Default to "unknown" if no category is provided to fall through ifs

    // List of all categories to print in order. Detection leads.
    const char *all_categories[] = {
        "detect", "wifi", "ble", "chameleon", "capture", "gps", "wigle",
        "comm", "sd", "led", "shell", "misc"
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

    if (strcmp(category, "detect") == 0) {
        glog("\nCounter-Surveillance Detection:\n\n");
        glog("sweep\n");
        glog("    Description: Full environment sweep - scans WiFi APs, stations and\n");
        glog("                 BLE devices, then saves a comprehensive report to SD card.\n");
        glog("    Usage: sweep [-w wifi_sec] [-b ble_sec]\n");
        glog("    Arguments:\n");
        glog("        -w  : WiFi scan duration per phase in seconds (default: 5)\n");
        glog("        -b  : BLE scan duration per phase in seconds (default: 5)\n");
        glog("    Output: /mnt/ghostesp/sweeps/sweep_N.csv\n\n");
        glog("flockscan\n");
        glog("    Description: Start Flock Safety surveillance-camera detection.\n");
        glog("    Usage: flockscan\n\n");
        glog("flocklist\n");
        glog("    Description: List detected surveillance devices (MAC, method,\n");
        glog("                 confidence, signal, channel, hits, SSID).\n");
        glog("    Usage: flocklist\n\n");
        glog("flockstop\n");
        glog("    Description: Stop Flock camera detection.\n");
        glog("    Usage: flockstop\n\n");
        glog("aerialscan\n");
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
        glog("pineap\n");
        glog("    Description: Detect WiFi Pineapple / KARMA-style rogue access points.\n");
        glog("    Usage: pineap [-s]\n");
        glog("    Arguments:\n");
        glog("        -s  : Stop PineAP detection\n\n");
        glog("wpa3check\n");
        glog("    Description: Audit AP security posture - WPA3 presence, transition\n");
        glog("                 mode, and PMF (management-frame protection).\n");
        glog("    Usage: wpa3check (after 'scanap', optionally 'select -a <index>')\n\n");
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
        glog("capture -skimmer\n");
        glog("    Description: Detect BLE credit-card skimmers.\n");
        glog("    Usage: capture -skimmer   (capture -stop to stop)\n\n");
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
        glog("See also: 'wpa3check', 'pineap', 'sweep' (help detect).\n\n");
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

    if (strcmp(category, "chameleon") == 0) {
        glog("\nChameleon Ultra Commands (reader / scan):\n\n");
        glog("chameleon connect [timeout] [pin]\n");
        glog("    Description: Connect to a Chameleon Ultra device via BLE\n");
        glog("    Usage: chameleon connect [timeout_seconds] [pin]\n");
        glog("    Arguments:\n");
        glog("        timeout_seconds : Connection timeout (default: 10)\n");
        glog("        pin            : PIN for authentication (4-6 digits, optional)\n\n");
        glog("chameleon disconnect\n");
        glog("    Description: Disconnect from the Chameleon Ultra device\n");
        glog("    Usage: chameleon disconnect\n\n");
        glog("chameleon status\n");
        glog("    Description: Check connection status with Chameleon Ultra\n");
        glog("    Usage: chameleon status\n\n");
        glog("chameleon scanhf\n");
        glog("    Description: Scan for High Frequency (HF) RFID tags\n");
        glog("    Usage: chameleon scanhf\n\n");
        glog("chameleon scanlf\n");
        glog("    Description: Scan for Low Frequency (LF) RFID tags\n");
        glog("    Usage: chameleon scanlf\n\n");
        glog("chameleon battery\n");
        glog("    Description: Get battery information from Chameleon Ultra\n");
        glog("    Usage: chameleon battery\n\n");
        glog("chameleon reader\n");
        glog("    Description: Set Chameleon Ultra to reader mode\n");
        glog("    Usage: chameleon reader\n\n");
        return;
    }
#endif

    if (strcmp(category, "capture") == 0) {
        glog("\nCapture Commands (passive):\n\n");
        glog("capture\n");
        glog("    Description: Start a passive WiFi capture (requires SD Card or Flipper).\n");
        glog("    Usage: capture [OPTION] [-channel <n>|-c <n>]\n");
        glog("    Arguments:\n");
        glog("        -probe     : Capture Probe packets\n");
        glog("        -beacon    : Capture Beacon packets\n");
        glog("        -deauth    : Capture Deauth packets (observe attacks)\n");
        glog("        -raw       : Capture Raw packets\n");
        glog("        -wps       : Capture WPS packets and their Auth Type\n");
        glog("        -pwn       : Capture Pwnagotchi packets\n");
        glog("        -eapol     : Capture EAPOL (handshake) packets\n");
        glog("        -list      : Browse saved PCAPs with +/- hc22000 markers\n");
        glog("        -export    : Export PCAP to hc22000 (PMKID + M2/M3)\n");
        glog("                    Usage: capture -export <pcap-file>\n");
        glog("        -wireshark : Stream raw PCAP to USB/UART for Wireshark\n");
        glog("                    Usage: capture -wireshark [-c <channel>|-channel <channel>]\n");
        glog("                    -channel <n>: Lock to specific channel (1-%d)\n", MAX_WIFI_CHANNEL);
        glog("        -wiresharkble : Stream BLE PCAP to USB/UART for Wireshark\n");
        #ifndef CONFIG_IDF_TARGET_ESP32S2
        glog("        -ble       : Start BLE packet capture\n");
        glog("        -skimmer   : Start skimmer (BLE) detection\n");
        #endif
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog("        -802154    : Capture IEEE 802.15.4 packets [C5/C6]\n");
        glog("                    Usage: capture -802154 [ch<n>|-channel <n>]\n");
        glog("                    -channel <n>: Lock to 802.15.4 channel (11-26)\n");
        #endif
        glog("        -stop      : Stops the active capture\n\n");
        glog("    -channel <n>: Lock the radio to channel <n> during the capture.\n");
        glog("                  Accepted by: -probe, -deauth, -beacon, -raw, -eapol, -pwn, -wps, -wireshark");
        #if defined(CONFIG_IDF_TARGET_ESP32C5) || defined(CONFIG_IDF_TARGET_ESP32C6)
        glog(", -802154 (11-26 only)");
        #endif
        glog(".\n\n");
        return;
    }

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

    if (strcmp(category, "comm") == 0) {
        glog("\nGhostLink / Communication Commands:\n\n");
        glog("commdiscovery\n    Check discovery status.\n    Usage: commdiscovery\n\n");
        glog("commconnect\n    Connect to a discovered peer ESP32.\n    Usage: commconnect <peer_name>\n    Example: commconnect ESP_A1B2C3\n\n");
        glog("commsend\n    Send a command to connected peer ESP32.\n    Usage: commsend <command> [data]\n    Example: commsend scanap\n\n");
        glog("commstatus\n    Show communication status.\n    Usage: commstatus\n\n");
        glog("commdisconnect\n    Disconnect from current peer.\n    Usage: commdisconnect\n\n");
        glog("commsetpins\n    Change communication GPIO pins at runtime.\n    Usage: commsetpins <tx_pin> <rx_pin>\n    Example: commsetpins 4 5\n\n");
#ifndef CONFIG_IDF_TARGET_ESP32S2
        glog("blebridge\n    Start/status/stop the BLE GhostLink bridge.\n    Usage: blebridge [start|stop|status|pair <peer_name>]\n\n");
#endif
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

    if (strcmp(category, "led") == 0) {
        glog("\nLED & RGB Commands:\n\n");
        glog("rgbmode\n    Control LED effects (rainbow, police, strobe, knight, off)\n    Usage: rgbmode <rainbow|police|strobe|knight|off|color>\n\n");
        glog("setrgbpins\n    Change RGB LED pins\n    Usage: setrgbpins <red> <green> <blue>\n           (use same value for all pins for single-pin LED strips)\n\n");
        glog("setrgbcount\n    Configure how many RGB LEDs are attached\n    Usage: setrgbcount <1-512>\n\n");
        glog("setneopixelbrightness\n    Set maximum neopixel brightness (percent)\n    Usage: setneopixelbrightness <0-100>\n\n");
        glog("getneopixelbrightness\n    Show current neopixel max brightness (percent)\n    Usage: getneopixelbrightness\n\n");
        return;
    }

    if (strcmp(category, "shell") == 0) {
        glog("\nHeadless Shell Commands:\n\n");
        glog("echo <text>                 Print text; supports \\n and \\t escapes.\n");
        glog("ifconfig                    Show STA and AP interfaces.\n");
        glog("ping <host> [count]         Send ICMP echo requests.\n");
        glog("scanlocal                   Discover hosts on the local network (mDNS).\n");
        glog("version                     Show firmware, build, git, and IDF versions.\n");
        glog("uuid | macaddr              Show stable device identifiers.\n");
        glog("uptime | date               Show uptime or current time.\n");
        glog("whoami | status             Show device identity or a system summary.\n");
        glog("hostname [name]             View or set the prompt hostname.\n");
        glog("color [name|0-255|off]      Set ANSI prompt color (also cli_color).\n");
        glog("banner [on|off|status]      Control the boot banner.\n");
        glog("clear                       Clear an ANSI terminal.\n");
        glog("alias [name command]        Create a persistent command shortcut.\n");
        glog("unalias <name|all>          Remove shortcuts.\n");
        glog("history [-c]                Show or clear command history.\n");
        glog("ps | top                   Show FreeRTOS task information.\n");
        glog("df                         Show /mnt filesystem usage.\n");
        glog("tail <file> [lines]         Print the end of an SD file.\n");
        glog("grep <pattern> <file>       Filter an SD file.\n");
        glog("source <file>               Run CLI commands from an SD file.\n");
        glog("tee <file> <text>           Append text to an SD file and echo it.\n");
        glog("env | export NAME=value     View or persist simple shell variables.\n");
        glog("watch <seconds> <command>  Repeat a command; use watch stop to end it.\n");
        glog("Unknown commands get a 'Did you mean?' suggestion automatically.\n\n");
        return;
    }

    if (strcmp(category, "misc") == 0) {
        glog("\nMiscellaneous Commands:\n\n");
        glog("help\n");
        glog("    Description: Display this help message.\n");
        glog("    Usage: help [category]\n\n");
#if CONFIG_ENABLE_GHOSTSCRIPT
        glog("script\n");
        glog("    Description: List, launch, monitor, or stop GhostScripts from the SD card.\n");
        glog("    Usage: script list | script run <index> | script status | script stop\n\n");
#endif
        glog("chipinfo\n");
        glog("    Description: Display chip information including model, revision, and features\n");
        glog("    Usage: chipinfo\n\n");
        glog("crash\n");
        glog("    Description: Intentionally trigger a crash (for coredump testing).\n");
        glog("    Usage: crash\n");
        glog("    The device will panic and save a coredump to flash; use idf.py coredump-info to inspect.\n\n");
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
        glog("coredump [dump|erase]\n");
        glog("    Description: Read or clear coredump in flash.\n");
        glog("    Usage: coredump        - Print summary (partition size, whether coredump present).\n");
        glog("           coredump dump   - Stream coredump as base64; save to file and run idf.py coredump-info -c <file> on host.\n");
        glog("           coredump erase  - Erase coredump partition (clears saved crash).\n\n");
#endif
        glog("timezone\n");
        glog("    Description: Set the display timezone for the clock view.\n");
        glog("    Usage: timezone <TZ_STRING>\n\n");
        glog("webauth\n");
        glog("    Description: Enable/disable web authentication.\n");
        glog("    Usage: webauth [on|off|toggle|status]\n\n");
        glog("statusidle\n");
        glog("    Description: View or change the status display idle animation (status OLED only).\n");
        glog("    Usage: statusidle [list|set <life|ghost|starfield|hud|matrix|ghosts|spiral|leaves|bouncing|0-8>]\n\n");
        glog("settings\n");
        glog("    Description: Manage NVS stored settings via command line\n");
        glog("    Usage: settings <command> [arguments]\n");
        glog("    Commands:\n");
        glog("        list                    - List all available settings\n");
        glog("        get <setting>           - Get current value of a setting\n");
        glog("        set <setting> <value>   - Set a setting to a value\n");
        glog("        reset [setting]         - Reset setting(s) to defaults\n");
        glog("    Examples:\n");
        glog("        settings list\n");
        glog("        settings get ap_ssid\n");
        glog("        settings reset\n\n");
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

    glog("  help detect    - Counter-surveillance detection (start here)\n");
    glog("  help wifi      - Wi-Fi scan & analyze commands\n");
#ifndef CONFIG_IDF_TARGET_ESP32S2
    glog("  help ble       - Bluetooth/BLE scan commands\n");
    glog("  help chameleon - Chameleon Ultra RFID reader\n");
#endif
    glog("  help capture   - Passive packet capture (WiFi/BLE/802.15.4)\n");
    glog("  help gps       - GPS & wardriving commands\n");
    glog("  help wigle     - WiGLE upload commands\n");
    glog("  help comm      - GhostLink / companion commands\n");
    glog("  help sd        - SD card commands\n");
    glog("  help led       - LED/RGB commands\n");
    glog("  help shell     - Headless shell commands\n");
    glog("  help misc      - Device & system commands\n");
#ifdef CONFIG_HAS_INFRARED
    glog("  help ir        - Infrared receive/learn commands\n");
#endif
    glog("  help all       - All commands\n\n");

    glog("Type 'help <category>' for details on that category.\n\n");
}
