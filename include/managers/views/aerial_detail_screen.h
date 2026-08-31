// aerial_detail_screen.h — dedicated drone detail: aircraft telemetry + the
// operator (takeoff) location decoded from DJI DroneID. Opened from the Drones
// drill-down list; set the target MAC before switching in.
#ifndef AERIAL_DETAIL_SCREEN_H
#define AERIAL_DETAIL_SCREEN_H

#include "managers/display_manager.h"

extern View aerial_detail_view;

void aerial_detail_set_mac(const char *mac);

#endif // AERIAL_DETAIL_SCREEN_H
