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
| Platform | core, system, settings, SD, display/LVGL, RGB, GPS, OTA | `main/core/*`, `managers/{settings,sd_card,display,rgb,ota}_manager.c`, `gui/*` |

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
- ✅ **Step 3 (BLE half) + Step 4 (BLE half) — BLE attack code removed, `attacks/ble/` deleted.** Removed: BLE spam wrappers (`ble_start_ble_spam`/`ble_stop_ble_spam`) and the `ble_spam_stop()`/`ble_stop_spoofing()` calls in `ble_deinit`/capture-stop from `ble_manager.c`; AirTag spoofing end-to-end — `airtag_scan_start_spoofing`/`airtag_scan_stop_spoofing`/`airtag_scan_spoof_device` + `airtag_spoofing_active` (`airtag_scan.c/.h`), `ble_start_spoofing_selected_airtag`/`ble_stop_spoofing` (`ble_manager.c/.h`), `ble_device_detect_start_airtag_spoof` (`device_detect_scan.c/.h` + S2 stub), the `selected_for_spoofing` struct field (both airtag structs) and the dead `selected_airtag_index` in `ble_manager.c`. UI: dropped the "Spoof" action button + `ble_detect_spoof_cb` and the orphaned `spoofairtag`/`stopspoof` menu branches in `options_screen.c`. Plugin API `ble_detect_start_airtag_spoof` **stubbed to return false** (struct member kept for plugin ABI stability). Dead `attacks/ble/ble_spam.h` includes removed from `cmd_ble.c`/`cmd_diagnostics.c`/`commandline.c`; S2 CMake filter for `attacks/ble` dropped. Deleted `main/attacks/ble/` + `include/attacks/ble/`. **KEPT** the environmental BLE-spam *detector* (`detect_ble_spam_callback`, `ble_start_blespam_detector`) and AirTag `select`/`listairtags` (inert selection/display). Verified: braces balanced across all 6 edited C files, zero remaining `ble_spam`/`attacks/ble`/spoof refs.
- ✅ **Step 3/4 (WiFi, part 1) — eapol/SAE/GTK/channel-switch/DHCP-starvation removed; 5 `attacks/wifi/` modules deleted.** Removed the `wifi_manager.c` wrappers (`wifi_manager_{start,stop}_eapollogoff_attack`/`eapollogoff_display`/`eapollogoff_help`, `wifi_manager_{start,stop}_sae_flood`/`sae_flood_help`, `wifi_manager_{start,stop,is}_gtk_abuse*`, `wifi_manager_{start,stop,is}_channel_switch_attack*`), their 4 includes, and their `wifi_manager.h` decls. Cleaned callers in `cmd_diagnostics.c` stop-all (channel-switch check + stop, `dhcp_starvation_stop`, eapol/sae stops) and dead `dhcp_starvation.h` includes in `cmd_scan.c`/`commandline.c`/`cmd_diagnostics.c` + the dead `gtk_abuse.h` include in `options_screen.c`. Deleted `attacks/wifi/{eapol_logoff,sae_flood,channel_switch_attack,gtk_abuse,dhcp_starvation}.{c,h}`. Braces balanced; ref sweep clean.
- ✅ **Plugin/Lua offensive TX primitives neutralized.** `plugin_api_lowlevel.c`: `plugin_api_wifi_raw_tx` (arbitrary 802.11 injection), `plugin_api_wifi_deauth`, `plugin_api_wifi_send_beacon` now return false without transmitting (no `esp_wifi_80211_tx`). `plugin_api.c` `ble_detect_start_airtag_spoof` stubbed earlier. **Struct members + signatures kept for plugin ABI stability** — capability removed, layout unchanged. (BLE `ble_adv_start` left intact = generic custom advertiser, consistent with kept `ble_start_custom_adv`.)
- ✅ **GhostChi REMOVED entirely (scope change: was KEEP → now REMOVE).** User decision: GhostChi is an automated WPA-handshake capture bot (opt-in deauth-burst "aggressive" mode) — removed wholesale rather than neutered. It was a pervasive Pwnagotchi-style mascot/gamification layer, not a standalone bot. Deleted files: `ghostchi_manager.{c,h}`, `ghostchi_mood.{c,h}`, `ghostchi_activity.{c,h}`, `views/ghostchi_screen.{c,h}`, `core/ghostchi_identity.{c,h}` (10 files). Severed integration across ~30 files: stripped all 30 `ghostchi_manager_add_xp()` XP hooks (sweep + fixed one orphaned empty `if` in `ble_manager.c` and one inline-conditional in `chameleon_manager.c`); removed 28 dead `#include`s; removed the status-bar **level badge** (`level_label` + click-to-open, `display_manager.c`); replaced espnow's `ghostchi_identity_get_name` with an inline `ESP-XXXX` MAC name; pinned the lockscreen companion sprite to a static image (dropped mood dependency + WAKE event); removed the "Ghostchi" app-gallery entry; removed the GhostChi SD dirs (`SD_DIR_GHOSTCHI*` + creation + `cmd_capture`/`options_screen` pcap-dir listings); removed `main.c` boot wiring (mood init/boot event, XP probe). **KEPT (documented deferral):** the 12 `plugin_api_canvas` `ghostchi/*` sprite entries + `vendor/images/ghostchi/*` assets (plugin-drawable bitmaps, non-offensive; removing all 12 would make an invalid empty array) and the `flappy_ghost` mini-game. Verified: all touched files brace-balanced (pre-existing `infrared_manager.c` string-brace imbalance confirmed present in HEAD), zero remaining `ghostchi` code refs outside the kept assets. **Now unblocked:** `deauth_attack` (GhostChi was its main caller).
- ✅ **Step 3/4 (WiFi, part 2) — deauth removed, `attacks/wifi/deauth_attack` deleted.** Removed all `wifi_manager` deauth wrappers (`start_deauth`, `deauth_station`, `auto_deauth`, `stop_deauth`, `stop_deauth_station`, `start/stop/is_running_handshake_deauth`) + their `wifi_manager.h` decls + the `deauth_attack.h` include; dropped the `main.c` `#ifdef USB_MODULE` auto-deauth boot block; removed the `cmd_diagnostics` stop-all deauth lines. Deleted `attacks/wifi/deauth_attack.{c,h}`. Braces balanced; ref sweep CLEAN. (`wifi_deauth_scan_callback` is a passive *capture* callback — kept, not an attack.)
- ✅ **Step 3/4 (WiFi, part 3) — karma removed, beacon-spam removed, `attacks/wifi/` EMPTY.** Deleted the whole **karma** subsystem in `wifi_manager.c` (statics, `karma_add_ssid`, `set_karma_ssid_list`, `set_karma_portal_file`, `karma_send_probe_response`, `karma_probe_request_callback`, `karma_start/stop_portal`, `karma_task`, `wifi_manager_{start,stop,is_running}_karma`, `KARMA_MAX_SSIDS`, `karma_portal_active`) + the beacon-spam wrappers (`broadcast_ap`, `start_beacon`, `stop_beacon`, beacon-list add/remove/clear/show/start) + their `wifi_manager.h` decls + `cmd_diagnostics` karma/beacon stop lines. Deleted `attacks/wifi/beacon_spam.{c,h}`. → **`attacks/wifi/` empty**; only `attacks/ethernet/eth_arp_poison` remains (goes with Ethernet in P2a).
- ✅ **Evil captive-portal server REMOVED.** Deleted the entire self-contained HTTP server from `wifi_manager.c` (~1180-line cluster: `start_portal_webserver` + ~30 httpd handlers incl. `portal_handler`/`get_log_handler`/`captive_portal_redirect_handler`/`file_handler`, `stream_data_to_client`, `start_evil_portal` body, `portal_stop_services_only`) **and** the credential/keystroke-harvest core (`CAPTURE_JS_SNIPPET` keylogger injection, `s_portal_creds_buf`/`s_portal_keystroke_buf`, `current_creds_filename`/`current_keystrokes_filename`, `portal_flush_buffers_to_sd`/`portal_force_flush_to_sd`, per-client HTTP rate-limit table, `portal_file_cache`, `evilportal_server`). Removed the `default_portal.h` include. The 4 public funcs (`start_evil_portal`→`ESP_ERR_NOT_SUPPORTED`, `stop_evil_portal`/`_keep_wifi`→no-op, `is_evil_portal_active`→false) kept as **inert stubs** so external callers (`display_manager` ×4, `cmd_shell`, `cmd_diagnostics`, `serial_manager` html-buffer) compile unchanged. Kept `html_buffer`/`MAX_HTML_BUFFER_SIZE` + the serial html-upload funcs (inert buffer mgmt, no portal to feed). Verified: `wifi_manager.c` braces 467/467, no portal-server/capture symbols remain. Build has no `-Werror`, so residual dead statics are safe. **KEPT for P4/P5 tidy (inert, cannot serve/capture):** `display_manager` `is_evil_portal_active()` branches (always false), `sd_card_manager` `/evil_portal` dir + `get_evil_portal_list`, `options_screen` portal picker UI (`evil_portal_names/options`, `portal_load_page`), `cmd_shell` "portal:" status line, `cmd_help` `evilportal` help text.
- ✅ **Step 3 (aerial) — drone emulation/spoof removed.** Deleted the emulation block in `aerial_detector_manager.c` (`is_emulating`/`emulation_*`/`emulated_*` statics, `encode_basic_id_message`, `encode_location_message`, `emulation_broadcast_callback`, `aerial_detector_{start,stop}_emulation`, `aerial_detector_is_emulating`, `aerial_detector_update_emulation_position`) + their header decls; trimmed the `cmd_diagnostics` stop-all to scanning-only; removed the "Spoof Test Drone"/"Stop Spoofing" menu entries + `aerialspoof`/`aerialspoofstop` UI branches in `options_screen.c`. Kept OpenDroneID **detection** (scan/list/track). Braces balanced; sweep clean. (`aerialspoof` commands were already unregistered in P1.)

### ✅ P1 COMPLETE — all offensive WiFi/BLE/aerial attack code removed; `attacks/{wifi,ble}` deleted.
`attacks/` now contains only `attacks/ethernet/eth_arp_poison` (removed with the Ethernet subsystem in P2a). Session totals so far: 26 files deleted, 44 modified — all brace/grep-verified, not compiled (build to confirm).

- ✅ **P2a — Ethernet toolset removed (whole subsystem + `attacks/ethernet/`).** Deleted 18 files: `ethernet_manager.{c,h}`, `managers/ethernet/{eth_comm_handler,eth_fingerprint,eth_http,eth_scan_async,eth_utils}.{c,h}`, `cmd_ethernet.c`, `views/ethernet_screen.{c,h}`, `attacks/ethernet/eth_arp_poison.{c,h}`, `vendor/images/ethernet.c`. → **`attacks/` tree now entirely gone.** Removed all `CONFIG_WITH_ETHERNET` `#ifdef` blocks + calls in kept files: `main.c` (init), `commandline.c` (16 `eth*` registrations + include block), `cmd_comm.c` (GhostLink eth handler), `cmd_diagnostics.c` (stop-all eth ARP-poison), `cmd_shell.c` (ETH ip/status), `cmd_help.c` (ethernet help section + category), `chip_info.c` (feature emit), `main_menu_screen.c` (menu item/nav/status/view-switch + icon declare), `options_screen.c` (GhostLink `dual_comm_ethernet_options` + `DUALCOMM_MENU_ETHERNET` + view switch), `commands.h` (17 decls), `display_manager.h` (`ethernet_screen_view` extern). Removed the Kconfig `WITH_ETHERNET` + 6 `ETH_W5500_*` options, the `espressif/w5500` component dep, the CMake ethernet block, and `CONFIG_WITH_ETHERNET=y` from `sdkconfig.somethingsomething2`. Simplified `sd_card_manager` SPI-host guards to NRF24-only. Plugin API `ethernet_is_connected`/`ethernet_ip` kept as inert `return false` stubs (ABI). Verified: all edited files brace- & preprocessor-balanced, zero residual eth-subsystem symbols. *(Deferred to P3: `# CONFIG_WITH_ETHERNET is not set` comment lines in the other ~38 board configs — harmless stale comments.)*
- ✅ **P2a — LAN recon removed (7 scan modules deleted).** Deleted `scans/wifi/{port_scan,arp_scan,ssh_scan,netbios_scan,http_banner_scan,snmp_scan,enum4linux_scan}.{c,h}`. Backend: removed the 7 recon handlers from `cmd_netscan.c` (kept `handle_congestion_cmd` + `handle_listen_probes_cmd`), all `wifi_manager.c` recon wrappers (`get_subnet_prefix`, `scanner_*`, `scan_ports_on_host`, `scan_ssh_on_host`, `scan_ip*_range`, `wifi_manager_scan_subnet`, `wifi_manager_arp_scan_subnet`, `scan_for_open_ports`) + their `wifi_manager.h` decls + `host_result_t`/`scanner_ctx_t` structs + `COMMON_PORTS` extern, the 7 `commandline.c` registrations (kept `scanlocal`/`enumgatt`), the 7 recon includes from `scans.h`, the `commands.h` handler+cancel decls, and the `cmd_diagnostics` stop-all cancel block. **UI (`options_screen.c`, the most entangled removal):** deleted the two async paged-list flows (ARP + Enum4linux) — statics, forward decls, `WIFI_MENU_ARP_LIST/DETAILS` + `WIFI_MENU_ENUM_LIST/DETAILS` enum values + all their switch cases, the shared detail-view input-dispatch branches (keyboard/joystick/encoder/back ×6), the scan-status cancel overlay, `close_all_scan_status_overlays`/menu-cleanup hooks, the paged-nav blocks, the two local start-flow handlers, and the `arp_scan.h`/`enum4linux_scan.h` includes; trimmed recon entries from `wifi_network_options`/`dual_comm_scan_options`/`dual_comm_tools_options` (kept mDNS Discovery). Verified: all 8 touched files brace-balanced, options_screen.c build-safe (2150/2150), zero repo-wide dangling recon refs. **KEPT:** `scanlocal`/`ifconfig`/`ping`, mDNS discovery, channel congestion, probe listening, and the detection scans (`airspace_monitor`, `ap_scan`, `station_scan`, `wpa3_compliance`). *(Deferred to P4: dead-but-compiling `simulateCommand`-based recon menu handlers + `*_kb_cb` keyboard callbacks in `options_screen.c` — unreachable, emit removed command strings, no C-symbol refs.)*
- ⏳ **P2a — remaining whole subsystems:** BadUSB, camera, audio, printer/cast.
- ⏳ **P2b** — IR TX/replay, SubGHz TX/replay, NFC/Chameleon emulation (keep RX/scan/read).
- ⏳ **P3** — drop `esp32`/Wroom board target.
- ⏳ **P4/P5 tidy** — `options_screen.c` dead menu-state tail (karma/portal/sinkhole/EVIL_PORTAL_SELECT UI + enums), `cmd_help.c` stale attack help text (`evilportal`, etc.), `sd_card_manager` evil_portal dir + `get_evil_portal_list`, `display_manager` dead `is_evil_portal_active()` branches, `cmd_shell` portal status line, `wifi_network_options` LAN-recon menu entries.
- ⏳ **Step 3 (aerial)** — `aerial_detector_manager.c` emulation/spoof section + drone-spoof UI (`options_screen.c` 2053/8122-8132, `aerialspoof`/`aerialspoofstop`).
- ⏳ **P2/P3/P5** — subsystem removals (BadUSB, Ethernet [+`attacks/ethernet/`], camera, audio, printer/cast, LAN recon), board/CI reduction, tidy (incl. `options_screen.c` dead menu-state tail).

## Constraints & notes

- **No compiler in the dev environment.** Changes are reference-traced (grep), not compiled here. Build to verify with IDF v6.0 or `build.py` after each phase.
- **"Matter detection" ≠ existing feature.** The repo has raw 802.15.4 capture only (no Matter/Thread stack). Matter/Thread/Zigbee all ride on 802.15.4; Matter-aware parsing is a future enhancement.
- **Companion app** for this fork is deferred until the teardown lands.
- Work branch: `countersurveillance` (forked from `dev`).
