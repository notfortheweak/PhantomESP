// scan_list_screen.h — Live Scan drill-down views.
//
// scan_list_view   : scrollable list of live signals for one category.
// scan_signal_view : full detail for a selected signal.
//
// The dashboard sets the category with scan_list_set_category() before switching
// to scan_list_view.
#ifndef SCAN_LIST_SCREEN_H
#define SCAN_LIST_SCREEN_H

#include "managers/display_manager.h"
#include "gui/scan_report.h"

extern View scan_list_view;
extern View scan_signal_view;

void scan_list_set_category(scan_category_id_t category);

#endif // SCAN_LIST_SCREEN_H
