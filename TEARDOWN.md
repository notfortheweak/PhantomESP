# Counter-Surveillance Fork — Teardown Plan

This document is the root planning reference for turning PhantomESP (a fork of GhostESP)
into a focused **detect & analyze counter-surveillance tool**. It records what we keep,
what we remove, and the order we do it in.

## Goal

A passive **detect-and-analyze** wireless counter-surveillance device. It observes and
classifies the RF environment (WiFi / BLE / 802.15.4) and geolocates observations via GPS.
It does **not** transmit attacks, spoof, or harvest credentials.

Top functions: **Flock/surveillance detection** and **drone detection**.

## Target hardware

- **MCU:** ESP32-C6 (WiFi 6, BLE 5, 802.15.4 radio for Zigbee/Thread/Matter).
- **GPS:** ATGM336H over UART (standard NMEA — uses the existing GPS driver).
- **Companion:** Flipper Zero via the GhostESP companion link (GhostLink). A dedicated
  companion app for this project is a **later** phase, after the teardown is complete.

This fork supports **all ESP32 C-series (C3, C5, C6) and S-series (S2, S3)** targets. The
original `esp32`/Wroom target is dropped. Removal is driven by **offensive vs. detect-and-analyze
scope**, not by which chip has which peripheral (the multi-board matrix is retained).

Primary reference target for bring-up: ESP32-C6 + ATGM336H GPS + Flipper (GhostLink).

> **S2 caveat:** ESP32-S2 has no BLE hardware, so BLE/AirTag detection does not exist on S2
> (consistent with upstream). An S2 node is WiFi + GPS detection only.

## Scope decisions (locked)

1. **All C-series + S-series boards** (C3/C5/C6, S2/S3). Drop only the `esp32`/Wroom target
   (this removes the CYD display family, Marauder v4/v6, JCMK DevBoardPro, Minion, FeberisPro —
   all `idf_target: esp32`).
2. **Remove active LAN recon** — nmap-style host probing is out of scope.
3. **Keep passive packet capture** — PCAP-to-SD and Wireshark streaming are "analyze".
4. **AirTag scanning is kept** (detection only; AirTag spoofing is removed).

---

## KEEP — detect & analyze (in scope)

| Area | What | Key files |
|---|---|---|
| WiFi recon | AP/STA scan, scanall, channel congestion, probe listen, PineAP detect, WPA3 compliance check, Airspace Monitor | `scans/wifi/*` (except LAN recon below), `wifi_manager.c` (scan half), `views/airspace_monitor_screen.c`, `views/channel_congestion_screen.c`, `views/packet_monitor_screen.c` |
| BLE recon | BLE scan (all modes), **AirTag scanning/detect** (`airtag_scan.c`, `listairtags`), Flipper finder, GATT enum/track, advertiser scan, device-detect, skimmer detect | `scans/ble/*`, `ble_manager.c` (scan half) |
| Flock detect ★ | Surveillance-camera / Flock detection | `flock_detector_manager.c`, `cmd_aerial.c` flock cmds |
| Drone detect ★ | OpenDroneID / aerial detection + tracking (spoof removed) | `aerial_detector_manager.c` (detect half), `cmd_aerial.c` |
| 802.15.4 | Passive Zigbee/Thread/Matter frame capture (RX only) | `zigbee_manager.c`, `cmd_capture.c` |
| GPS / wardriving | gpsinfo/gpspin/gpsbaud, startwd, wdstream, WiGLE export | `gps_manager.c`, `wigle_manager.c`, `cmd_gps.c`, `cmd_wdstream.c`, `cmd_wigle.c`, `vendor/GPS/*`, `views/wardriving_screen.c` |
| Capture | Passive PCAP-to-SD + Wireshark streaming (WiFi/BLE/802.15.4) | `cmd_capture.c`, `vendor/pcap.c` |
| GhostLink | Flipper companion link, BLE bridge | `esp_comm_manager.c`, `ble_bridge_manager.c`, `cmd_comm.c` |
| Tracking | trackap / tracksta / RSSI meter | `wifi_manager.c`, `gui/rssi_meter.c` |
| Platform | core, system, settings, SD, display/LVGL, RGB, GPS, OTA, GhostChi | `main/core/*`, `managers/{settings,sd_card,display,rgb,ota}_manager.c`, `gui/*` |

## REMOVE — offensive or out-of-scope

### 1. Dedicated attack tree (delete whole)
- `main/attacks/**` and `include/attacks/**` (WiFi deauth/beacon-spam/channel-switch/DHCP-starve/EAPOL-logoff/GTK-abuse/SAE-flood, BLE spam, Ethernet ARP-poison).

### 2. Attack logic embedded in kept managers (surgical)
- `wifi_manager.c` — remove deauth, beacon spam, karma, EAPOL logoff, SAE flood, channel-switch, GTK abuse, and the **evil captive portal** (credential/keystroke capture buffers). Keep the scanning/tracking half.
- `ble_manager.c` — remove BLE spam + AirTag spoofing. Keep scan.
- `aerial_detector_manager.c` — remove the emulation/spoof section. Keep detection.
- `ap_manager.c` — remove evil-portal serving path. Keep the AP used by WebUI/GhostLink.

### 3. Offensive commands (handlers + `register_command` lines)
`attack`, `beaconspam`/`beaconadd`/`beaconremove`/`beaconclear`/`beaconshow`/`beaconspamlist`,
`stopspam`, `stopdeauth`, `dhcpstarve`, `saeflood`/`stopsaeflood`/`saefloodhelp`, `blespam`,
`karma`, `spoofairtag`/`stopspoof`, `startportal`/`stopportal`/`evilportal`/`listportals`,
`aerialspoof`/`aerialspoofstop`, `ethpoison`.

### 4. Out-of-scope / offensive subsystems removed whole (all boards)
- **BadUSB / USB-HID injection**: `badusb_manager.c`, `hid_script_parser.c`, `usb_keyboard_manager.c`, `cmd_badusb.c`, `views/badusb_view.c`, `views/trackpad_view.c`.
- **Ethernet / W5500 toolset** (active recon + MITM/ARP-poison): `ethernet_manager.c`, `managers/ethernet/**`, `cmd_ethernet.c`, `views/ethernet_screen.c`, `attacks/ethernet/**`.
- **Camera** (not counter-surveillance): `camera_stream_manager.c`, `motion_detector_manager.c`, `cmd_camera.c`, `views/{...camera...}`, `esp32-camera` component.
- **Audio** (fluff): `audio_*_manager.c`, `microphone/**`, `cmd_audio.c`, `views/{audio_player,music_visualizer}_screen.c`, audio codec components.
- **Printer / cast hijack**: `printer.c` (`powerprinter`), `dial_manager.c` (`dialconnect`/`tplinktest`).

### 4b. Mixed subsystems KEPT — but offensive halves stripped
- **NRF24** — keep whole (`nrf24_remote_manager.c`, `cmd_nrf24.c`, `views/nrf24_analyzer_view.c`): 2.4 GHz spectrum analyzer + passive jamming detection. Already detection-only.
- **IR — RX/learn only**: keep `infrared_common.c`, `infrared_decoder.c`, `infrared_protocols.c`, `infrared_rx_gpio.c`, receive path of `infrared_manager.c`, `views/infrared_view.c`, RX cmds. **Remove** `infrared_protocol_encoders.c`, `universal_ir.c`, and all TX/transmit/replay paths (`cmd_ir.c` send subcommands).
- **SubGHz — scan/waterfall only**: keep spectrum scan + `subghz_decoders.c` (decode) + `views/subghz_view.c` scan UI. **Remove** transmit/replay of `.sub` signals and the TX paths in `subghz_remote_manager.c` / `cmd_subghz.c`.
- **NFC — read/parse only**: keep PN532/ST25R3916 read + Flipper parsers (`managers/nfc/**`, `views/nfc_view.c`, `esp-idf-pn532`, `esp-idf-st25r3916`) and Chameleon **reader/scan** functions. **Remove** Chameleon card **emulation/spoofing** paths in `chameleon_manager.c` and any NFC write/emulate commands.

### 5. Active LAN recon (removed by decision)
`scans/wifi/{port_scan,arp_scan,ssh_scan,netbios_scan,http_banner_scan,snmp_scan,enum4linux_scan}.c`
+ commands `scanports`/`scanarp`/`scanssh`/`netbiosscan`/`httpbannerscan`/`snmpprobe`/`enumscan`
(`cmd_netscan.c`). Keep `scanlocal`/`ifconfig`/`ping` basic connectivity.

### 6. Board configs + CI matrix
Keep every **C-series and S-series** board config (idf_target esp32s2/esp32s3/esp32c3/esp32c5/esp32c6).
Remove only the **`idf_target: esp32`** boards from `configs/`, `build.py get_build_targets()`,
and `.github/workflows/compile_all.yml`: esp32-generic, MarauderV4_FlipperHub, MarauderV6&AwokDual,
the CYD family (CYD2USB/CYDMicroUSB/CYDDualUSB/CYD2USB2.4*/CYD2432S028R), JCMK_DevBoardPro,
RabbitLabs_Minion, FeberisPro. Then clean `main/idf_component.yml` rules and `main/CMakeLists.txt`
filters that only served removed subsystems.

> **Keep (not offensive):** `apcred` sets the device's *own* AP password — infrastructure, not credential harvesting.

---

## Execution phases (build-verify after each)

- **P1 — Offensive commands & attack tree.** Delete `attacks/**` + `include/attacks/**`; remove offensive `register_command` lines and their handlers/`cmd_*` files; strip attack includes/calls from `wifi_manager.c`, `ble_manager.c`, `ap_manager.c`, `aerial_detector_manager.c`.
- **P2a — Remove whole subsystems.** BadUSB, Ethernet toolset, camera, audio, printer/cast, LAN recon (sources, commands, views, components, includes).
- **P2b — Strip offensive halves of kept subsystems.** IR TX/replay, SubGHz TX/replay, NFC/Chameleon emulation. Keep IR-RX, SubGHz scan, NFC read, NRF24 (whole).
- **P3 — Drop `esp32`/Wroom target.** Remove `idf_target: esp32` board configs from `configs/`, `build.py`, `compile_all.yml`; keep all S/C-series. Clean `idf_component.yml` + CMake filters.
- **P4 — UI/menu & help.** Remove dead menu entries in `main_menu_screen.c` / `options_screen.c`, dead views, and stale `cmd_help.c` text.
- **P5 — Tidy pass.** Re-review for dangling refs, dead includes, orphaned assets; update `README.md` / `CHANGELOG.md` / `CLAUDE.md`.

## Progress log

- ✅ **P1 (Step 1) — CLI attack commands removed.** 23 registrations + handlers across `cmd_wifi/scan/ble/aerial/portal.c`, whole `cmd_karma.c`, and `commands.h` decls. Verified: brace-balanced, kept commands intact, zero dangling handler symbols. (`ethpoison` deferred to Ethernet removal.)
- ✅ **DNS sinkhole (command + parent menu) removed.** Scope change: sinkhole is ad/tracker blocklist blocking, not counter-surveillance — cut. Deleted `cmd_portal.c` (whole; it was 100% sinkhole after the evil-portal handlers came out), its registration, and `commands.h` decl; dropped "DNS Sinkhole" from `wifi_main_options`. Dead sinkhole UI plumbing in `options_screen.c` (`sinkhole_detail_view`, `WIFI_MENU_DNS_SINKHOLE*`, blocklist helpers) + the `dns_sinkhole_*` backend in `dns_server.c` are now unreferenced — cleaned in Step 2b / Step 3.
- ✅ **P4 partial (Step 2a) — attacks unreachable in UI.** `options_screen.c`: deleted pure-attack arrays (`wifi_attacks_options`, `wifi_evil_portal_options`, `dual_comm_attacks_options`, `wifi_misc_options`) + their nav-cases; trimmed attack entries from `wifi_main_options` and the `dual_comm_*` remote menus (spoof/spam/evil-portal/cast/printer). AirTag scan + Flipper finder kept. Verified brace-balanced, no dangling array refs.
- 🟡 **Step 2b (in progress) — `options_screen.c` plumbing.** Removed: both dispatch chains (local + GhostLink `commsend`) for all attacks/sinkhole/dial/printer/portal; ~30 scattered `detail_view` input-handler branches (scroll/touch/encoder/keyboard/back), the compound-OR touch condition, the contiguous attack-helper callback block (`gtk_abuse_*`, karma, `sae_flood_password_cb`), and the `KARMA_PORTAL_SELECT` picker block; orphaned `gtk_abuse` statics/decls. Brace- & preprocessor-balanced after every batch. **Still dead-but-compiling (tidy tail):** `EVIL_PORTAL_SELECT` flow (+`portal_load_page`/`selected_portal`), `sinkhole_detail_back_cb` + its static + one call site, `blocklist_*` helpers, menu-state enums (`WIFI_MENU_ATTACKS/EVIL_PORTAL/MISC/*_SELECT/DNS_SINKHOLE*`, `DUALCOMM_MENU_ATTACKS`), their nav-cases in both switches, `dual_comm_karma_custom_ssids_cb`, leftover attack `#include`s. Note: grep-only surfaced a live dangling ref (`karma_portal_ssids_cb` call) by luck — a compiler would flag the rest instantly.
- ⏳ **Step 3** — manager attack wrappers (`wifi_manager.c`, `ble_manager.c`, `ap_manager.c`, `aerial_detector_manager.c`).
- ⏳ **Step 4** — delete `attacks/**` tree.
- ⏳ **P2/P3/P5** — subsystem removals, board/CI reduction, tidy.

## Constraints & notes

- **No compiler in the dev environment.** Changes are reference-traced (grep), not compiled here. Build to verify with IDF v6.0 or `build.py` after each phase.
- **"Matter detection" ≠ existing feature.** The repo has raw 802.15.4 capture only (no Matter/Thread stack). Matter/Thread/Zigbee all ride on 802.15.4; Matter-aware parsing is a future enhancement.
- **Companion app** for this fork is deferred until the teardown lands.
- Work branch: `countersurveillance` (forked from `dev`).
