// wardrive_list_screen.h — wardriving drill-down views.
//
// wardrive_list_view   : scrollable list of wardriven networks for one kind.
// wardrive_signal_view : full per-network detail (incl. GPS coords).
//
// The dashboard sets the kind with wardrive_list_set_kind() before switching in.
#ifndef WARDRIVE_LIST_SCREEN_H
#define WARDRIVE_LIST_SCREEN_H

#include "managers/display_manager.h"
#include "gui/wardrive_report.h"

extern View wardrive_list_view;
extern View wardrive_signal_view;

void wardrive_list_set_kind(wd_kind_t kind);

#endif // WARDRIVE_LIST_SCREEN_H
