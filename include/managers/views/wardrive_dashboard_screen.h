// wardrive_dashboard_screen.h — Live-Scan-style wardriving dashboard.
//
// A tile dashboard (networks / logged / speed / GPS) over a compact GPS header;
// tap the networks tile to drill into the live list of wardriven networks
// (wardrive_list_view) and per-network detail. Owns the wardriving engine + CSV
// for the session. Select the radio with wardrive_dashboard_set_ble_mode()
// before switching in (WiFi mode when false).
#ifndef WARDRIVE_DASHBOARD_SCREEN_H
#define WARDRIVE_DASHBOARD_SCREEN_H

#include <stdbool.h>
#include "managers/display_manager.h"

extern View wardrive_dashboard_view;

void wardrive_dashboard_set_ble_mode(bool enabled);

#endif // WARDRIVE_DASHBOARD_SCREEN_H
