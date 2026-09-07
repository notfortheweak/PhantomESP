# PhantomESP

> **A counter-surveillance-focused firmware for ESP32, based on GhostESP.**

PhantomESP is a fork of [GhostESP](https://github.com/GhostESP-Revival/GhostESP) reoriented
around **detection and situational awareness** rather than offense: spotting surveillance
cameras, drones/Remote-ID, rogue access points and trackers, and mapping the RF environment.
It runs on the same ESP32-family boards with a full touchscreen LVGL UI, a serial CLI, and a
web UI.

Built directly on Espressif's **ESP-IDF v6.0** (not Arduino/PlatformIO). A single codebase
targets ~46 boards; per-board differences live in `configs/sdkconfig.*`.

## Highlights

- **Counter-surveillance detection**: full environment sweep, Flock Safety camera detection,
  WiFi Pineapple / rogue-AP detection, WPA3/SAE compliance checks.
- **Aerial / drone detection**: OpenDroneID / Remote-ID and DJI drone detection over WiFi + BLE,
  with tracking and diagnostics.
- **BLE awareness**: AirTag/tracker, Flipper, and GATT device discovery; advertiser scanning.
- **GPS + wardriving**: WiFi/BLE wardriving with GPS logging and WiGLE upload, plus a serial
  companion telemetry stream (`wdstream`).
- **Radios & IO**: NFC (PN532 / ST25R3916), IR, SubGHz, NRF24, Ethernet, USB HID.
- **Platform**: 60 FPS LVGL UI, native SD apps, and a sandboxed Lua scripting runtime.

## Build & flash

Source ESP-IDF v6.0 first (`. $IDF_PATH/export.sh`), then either use the interactive helper:

```bash
python3 build.py                 # interactive board picker
python3 build.py --targets all   # build every target into local_builds/
```

or build a board manually by copying its config into place:

```bash
rm -f sdkconfig sdkconfig.defaults
cp configs/sdkconfig.<board> sdkconfig.defaults
cp configs/sdkconfig.<board> sdkconfig
idf.py set-target <esp32|esp32s3|esp32c5|...>   # match the board's CONFIG_IDF_TARGET
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

CI (`.github/workflows/compile_all.yml`) builds all boards; `tools/local_build_check.sh <board>`
reproduces a single-board CI build locally.

## Credits & license

PhantomESP is **based on GhostESP** (and the original Ghost ESP project). Firmware is licensed
under **GPL-3.0** — see [`LICENSE`](LICENSE). Upstream and original authors retain credit for the
GhostESP codebase this fork builds on; PhantomESP is an independent, community fork and is not
affiliated with or endorsed by the GhostESP maintainers.
