# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

GhostESP (repo name PhantomESP) is an ESP-IDF firmware for ESP32-family chips: a wireless-research / security platform covering WiFi, BLE, NFC, IR, SubGHz, NRF24, Ethernet, GPS, USB HID, and 802.15.4/Zigbee, plus a full LVGL graphical UI, a native SD-app ecosystem, and a Lua scripting runtime. It is **ESP-IDF-native** (built directly on Espressif's SDK), not Arduino/PlatformIO. Toolchain: **ESP-IDF v6.0** (this is what CI builds against; `build.py`'s auto-download default of 5.5.1 is a fallback — target v6.0).

A single codebase compiles for **~46 board targets**. Board differences (pins, peripherals, features) are expressed entirely through Kconfig options baked into per-board `configs/sdkconfig.*` files — there is no per-board source directory. Code branches on `CONFIG_*` symbols and on `IDF_TARGET`.

## Build & flash

The build always works by copying a board's config into place, setting the target, and running `idf.py`. Do it manually (matches CI) or via `build.py`.

Manual (source `. $IDF_PATH/export.sh` first):
```bash
rm -f sdkconfig sdkconfig.defaults
cp configs/sdkconfig.cardputer sdkconfig.defaults   # pick the target board's config
cp configs/sdkconfig.cardputer sdkconfig
idf.py set-target esp32s3                            # must match the board's idf_target
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```
The `idf_target` for each config (esp32 / esp32s2 / esp32s3 / esp32c3 / esp32c5 / esp32c6) is listed in `build.py` `get_build_targets()` and in `.github/workflows/compile_all.yml` — always pair the right target with the config, or the build breaks.

Interactive helper (auto-downloads ESP-IDF, presents a numbered board menu):
```bash
python3 build.py                 # interactive board picker
python3 build.py --targets all   # build every target into local_builds/
python3 build.py --targets 11    # build one by index (indices are get_build_targets() order)
```

There is **no test suite, linter, or unit-test target** in this repo. "Does it build" is the correctness gate; CI (`compile_all.yml`) builds all boards. When changing shared code, sanity-check that it still compiles for a representative board of each chip family (esp32, esp32s2 — no BLE, esp32s3, esp32c5/c6), since guards differ per target.

## Per-target compilation gotchas

`main/CMakeLists.txt` globs `main/**/*.c` but then surgically filters — read it before adding/moving files:
- **ESP32-S2 has no BLE hardware**: `attacks/ble/`, `scans/ble/`, and `ble_manager.c` are excluded for `esp32s2`, and `main.c` guards BLE includes with `#ifndef CONFIG_IDF_TARGET_ESP32S2`. New BLE code must sit under those paths or be guarded the same way.
- **Flipper NFC parsers** (`main/managers/nfc/flipper_parsers/*.c`) are excluded from the glob and individually whitelisted via `list(APPEND ...)`. A new parser is not compiled until you add it to that list.
- Several files are conditionally included: `infrared_view.c`, `i80_display.c`, camera managers (`motion_detector_manager.c`, `camera_stream_manager.c`). Check the CMake conditionals before assuming a file is in the build.

## Architecture

Firmware entry is `main/main.c`. The codebase is organized by role, split across `main/` (sources) and `include/` (headers) with mirrored subdirectory trees:

- **`main/core/`** — command dispatch, serial/shell, system/DNS, OUI lookup, crypto, logging (`glog`). `commandline.c` holds a command registry: `register_command(name, fn)` populates a table, and `register_commands()` (~line 251) wires every command name to its handler. Handlers live in **`main/core/commands/cmd_*.c`**, grouped by domain (`cmd_wifi.c`, `cmd_ble.c`, `cmd_nfc.c`, `cmd_subghz.c`, `cmd_ethernet.c`, `cmd_badusb.c`, `cmd_script.c`, …). Adding a CLI command = write a handler in the relevant `cmd_*.c` and register it in `register_commands()`.
- **`main/managers/`** — the bulk of the system: one manager per subsystem (`wifi_manager`, `ble_manager`, `gps_manager`, `display_manager`, `sd_card_manager`, `settings_manager`, `ota_manager`, `plugin_manager`, `ghostscript_*` Lua runtime, NFC/`nfc/`, Ethernet/`ethernet/`, RGB/`rgb_effects/`, `views/` for LVGL screens). Managers own hardware and long-running tasks; commands and UI call into them.
- **`main/attacks/`** and **`main/scans/`** — offensive and reconnaissance operations, further split by radio (`attacks/ble/`, `scans/ble/`, etc.).
- **`main/gui/`** — LVGL UI primitives (menus, toasts, popups, theme palette, RSSI meter, charts). Board views are in `main/managers/views/`.
- **`main/vendor/`** — third-party/hardware glue: display drivers (`CH422G`, `ST7262`, `axs15231b`, `i80_display`), GPS, `led/`, `m5/` keyboard, `pcap.c`, `printer.c`, `drivers/` (e.g. `pcf8563` RTC).
- **`components/`** — vendored ESP-IDF components (LVGL, `lua`, `esp-idf-pn532`, `esp-idf-st25r3916`, `esp32-camera`, audio codecs, display drivers). External IDF-registry deps are in `main/idf_component.yml` (many gated by `rules: if: target in [...]`).

The device is controllable through multiple front-ends that all funnel into the same command/manager layer: on-device LVGL UI, serial CLI, WebUI (`webui/`), Flipper app, Android companion, and **GhostLink** (a dual-ESP32 command/display bridge; see `esp_comm_manager.c`, `peer_ota_manager.c`, `ble_bridge_manager.c`).

## Native SD apps / plugins

Loadable apps live under `plugins/` (SDK in `plugins/sdk/`, `package.schema.json`, examples, and the **`gbt`** Ghost Build Tool). At compile time the feature depends on `CONFIG_SPIRAM`; at runtime it requires PSRAM (`MALLOC_CAP_SPIRAM`) and a display. The on-device runtime is `plugin_*` managers plus the `elf_loader` component; C5 can run app code from flash (XIP). Lua scripting is the `ghostscript_*` managers backed by the `lua` component.

## Partitions & OTA

Multiple `partitions*.csv` at the repo root cover different flash sizes and OTA layouts (e.g. `partitions_ota_16mb.csv`, `partitions_ota_8mb.csv`, `partitions_c5_xip.csv`). Each board's `sdkconfig.*` selects its partition CSV. OTA-capable boards are flagged with `ota: true` + `ota_slot_size` + `board_key` in `compile_all.yml`; `firmware-manifest.json` (generated) drives the OTA updater's version comparisons, using the monotonic `GHOSTESP_BUILD_NUMBER` compile definition (from CI run number) rather than the git-describe version.

## Conventions

- Branch from `Development-deki` (the effective main branch) for PRs; the working branch is `dev`.
- Match the surrounding file's style. Guard target-specific code with the exact `CONFIG_*` / `CONFIG_IDF_TARGET_*` symbols already used nearby rather than inventing new ones.
- Prefer `heap_caps_*` with explicit caps (PSRAM vs internal) as the existing managers do; check allocations.
- User-facing history goes in `CHANGELOG.md` (large, actively maintained).
