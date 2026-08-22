// scan_dashboard_screen.h
// Live threat dashboard: a grid of category tiles (Drones, Flock, PineAP,
// Flippers, AirTags, BLE, WiFi) polled off the detection engines while the
// multi-radio scan scheduler cycles them. This is the auto-boot landing view.
#pragma once

#include "managers/display_manager.h"

#ifdef __cplusplus
extern "C" {
#endif

extern View scan_dashboard_view;

#ifdef __cplusplus
}
#endif
