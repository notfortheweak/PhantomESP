// scan_dashboard_screen.c — see header. Live threat dashboard: a tappable tile
// per category. Tapping a tile drills into its live signal list
// (scan_list_view); the "Menu" button (or hardware back) returns to the main
// menu. The scan scheduler keeps running while drilling into sub-views — it is
// stopped only when leaving Live Scan for the main menu.
#include "managers/views/scan_dashboard_screen.h"

#include "sdkconfig.h"
#include "lvgl.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include "gui/scan_tile.h"
#include "gui/scan_report.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/scan_scheduler.h"
#include "managers/settings_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/scan_list_screen.h"

#include "core/callbacks.h"            // pineap_get_detected_count
#include "managers/aerial_detector_manager.h"
#include "managers/flock_detector_manager.h"
#include "scans/wifi/ap_scan.h"
#include "scans/wifi/station_scan.h"
#include "scans/wifi/camera_detect.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "scans/ble/flipper_scan.h"
#include "scans/ble/airtag_scan.h"
#include "scans/ble/device_detect_scan.h"
#endif

static lv_obj_t *s_root = NULL;
static lv_obj_t *s_content = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_timer_t *s_timer = NULL;

#define MAX_TILES 8
static struct { scan_tile_t *tile; scan_category_id_t cat; } s_tiles[MAX_TILES];
static int s_ntiles = 0;

// touch tracking
static bool s_touch_started = false, s_touch_dragged = false;
static int s_sx, s_sy, s_lx, s_ly;

// Live Scan keeps the screen always-on for uninterrupted observability; we
// stash the user's display timeout on entry and restore it when leaving.
static uint32_t s_saved_timeout = 0;
static bool s_screen_forced = false;

static void add_tile(lv_obj_t *content, const char *label, const char *icon,
                     scan_category_id_t cat) {
    scan_tile_t *t = scan_tile_create(content, label, icon);
    if (!t) return;
    lv_obj_set_width(scan_tile_get_obj(t), LV_PCT(48));
    if (s_ntiles < MAX_TILES) { s_tiles[s_ntiles].tile = t; s_tiles[s_ntiles].cat = cat; s_ntiles++; }
}

// Threat categories glow red on any hit; presence categories glow amber.
static scan_severity_t sev_for(scan_category_id_t cat, int count) {
    if (count <= 0) return SCAN_SEV_IDLE;
    switch (cat) {
    case SCAT_DRONES:
    case SCAT_FLOCK:
    case SCAT_PINEAP:
    case SCAT_FLIPPERS:
        return SCAN_SEV_THREAT;
    case SCAT_CAMERAS:
        // Ordinary IP cameras are everywhere, so they stay amber; only targeted
        // surveillance platforms (ALPR / bodycam / cloud) escalate to red, which
        // keeps a Flock or Axon hit from being lost among shop cameras.
        return camera_detect_get_targeted_count() > 0 ? SCAN_SEV_THREAT
                                                      : SCAN_SEV_PRESENT;
    default:                    // WiFi, AirTags, BLE
        return SCAN_SEV_PRESENT;
    }
}

// Counts come from the session accumulator (fed by the scheduler task), NOT the
// live engine getters — so the LVGL task never races the scheduler's scans, and
// tiles show active-now plus a running "N seen" session total. WiFi splits into
// A:<access points> S:<stations>.
static void update_cb(lv_timer_t *timer) {
    (void)timer;
    // Flip each tick so the active tile alternates -> a ~1Hz pulse at 500ms.
    static bool pulse_phase = false;
    pulse_phase = !pulse_phase;
    int scanning_cat = scan_scheduler_current_category();

    char primary[24];
    for (int i = 0; i < s_ntiles; i++) {
        scan_category_id_t cat = s_tiles[i].cat;
        scan_tile_set_scanning(s_tiles[i].tile, (int)cat == scanning_cat, pulse_phase);
        int active = scan_report_active_count(cat);
        int total = scan_report_total_count(cat);
        if (cat == SCAT_WIFI) {
            snprintf(primary, sizeof(primary), "A:%d S:%d",
                     scan_report_kind_active(cat, SKIND_AP),
                     scan_report_kind_active(cat, SKIND_STATION));
        } else {
            snprintf(primary, sizeof(primary), "%d", active);
        }
        scan_tile_set(s_tiles[i].tile, primary, total, sev_for(cat, active));
    }
}

static void scan_dashboard_create(void) {
    if (scan_dashboard_view.root != NULL) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t bg = theme_palette_get_background(theme);
    uint32_t accent = theme_palette_get_accent(theme);
    uint32_t text = theme_palette_get_text(theme);

    // Force the screen to stay on for the whole Live Scan session (guarded so
    // re-entering from a drill-down doesn't overwrite the saved value with the
    // already-forced "Never"). Restored in go_to_menu().
    if (!s_screen_forced) {
        s_saved_timeout = G_Settings.display_timeout_ms;
        G_Settings.display_timeout_ms = UINT32_MAX;   // UINT32_MAX == "Never"
        s_screen_forced = true;
        scan_report_reset_session();   // fresh session totals per Live Scan visit
    }
    scan_scheduler_set_focus(-1);   // dashboard shows all categories (round-robin)

    s_ntiles = 0;
    display_manager_fill_screen(lv_color_hex(bg));
    s_root = gui_screen_create_root(NULL, "Live Scan", lv_color_hex(bg), LV_OPA_COVER);
    scan_dashboard_view.root = s_root;

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
    s_content = content;
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_style_pad_column(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    // Full-width "Menu" back button on its own row.
    s_back_btn = lv_obj_create(content);
    lv_obj_set_width(s_back_btn, LV_PCT(100));
    lv_obj_set_height(s_back_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(accent), 0);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_set_style_radius(s_back_btn, 4, 0);
    lv_obj_set_style_pad_ver(s_back_btn, 5, 0);
    lv_obj_set_style_pad_hor(s_back_btn, 8, 0);
    lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Menu");
    lv_obj_set_style_text_color(bl, lv_color_hex(text), 0);

    add_tile(content, "WiFi",      LV_SYMBOL_WIFI,      SCAT_WIFI);
    add_tile(content, "Drones",    LV_SYMBOL_UP,        SCAT_DRONES);
    add_tile(content, "Cameras",   LV_SYMBOL_VIDEO,     SCAT_CAMERAS);
    add_tile(content, "Flock Cam", LV_SYMBOL_EYE_OPEN,  SCAT_FLOCK);
    add_tile(content, "PineAP",    LV_SYMBOL_WARNING,   SCAT_PINEAP);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    add_tile(content, "Flippers",  LV_SYMBOL_USB,       SCAT_FLIPPERS);
    add_tile(content, "AirTags",   LV_SYMBOL_GPS,       SCAT_AIRTAGS);
    add_tile(content, "BLE",       LV_SYMBOL_BLUETOOTH, SCAT_BLE);
#endif

    scan_scheduler_start();   // idempotent; keeps running across drill-in
    s_timer = lv_timer_create(update_cb, 500, NULL);
}

static void scan_dashboard_destroy(void) {
    // NOTE: intentionally does NOT stop the scheduler — drilling into a category
    // destroys this view but scanning must continue. The scheduler is stopped
    // only in go_to_menu().
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    for (int i = 0; i < s_ntiles; i++) scan_tile_destroy(s_tiles[i].tile);
    s_ntiles = 0;
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = s_content = s_back_btn = NULL;
    scan_dashboard_view.root = NULL;
    s_touch_started = false;
}

static void go_to_menu(void) {
    if (s_screen_forced) {
        G_Settings.display_timeout_ms = s_saved_timeout;   // restore sleep behavior
        s_screen_forced = false;
    }
    scan_scheduler_stop();
    display_manager_switch_view(&main_menu_view);
}

static void open_category(scan_category_id_t cat) {
    scan_list_set_category(cat);
    scan_scheduler_set_focus((int)cat);   // fast-scan just this category while viewing
    display_manager_switch_view(&scan_list_view);   // scheduler keeps running
}

static bool point_in(lv_obj_t *obj, int x, int y) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static void scan_dashboard_input(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_started) {
                s_touch_started = true; s_touch_dragged = false;
                s_sx = s_lx = d->point.x; s_sy = s_ly = d->point.y;
            } else {
                int dy = d->point.y - s_ly;
                s_lx = d->point.x; s_ly = d->point.y;
                if (abs(d->point.y - s_sy) > 8 || abs(d->point.x - s_sx) > 8)
                    s_touch_dragged = true;
                if (s_touch_dragged && s_content && dy)
                    display_manager_queue_scroll(s_content, dy);
            }
        } else if (d->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            if (s_touch_dragged) return;
            int x = d->point.x, y = d->point.y;
            if (point_in(s_back_btn, x, y)) { go_to_menu(); return; }
            for (int i = 0; i < s_ntiles; i++) {
                if (point_in(scan_tile_get_obj(s_tiles[i].tile), x, y)) {
                    open_category(s_tiles[i].cat);
                    return;
                }
            }
        }
        return;
    }

    switch (event->type) {
    case INPUT_TYPE_EXIT_BUTTON:
        go_to_menu();
        break;
    case INPUT_TYPE_KEYBOARD: {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == 27 || key == 29 || key == '`' ||
            key == 'q' || key == 'Q' || key == LV_KEY_LEFT)
            go_to_menu();
        break;
    }
    case INPUT_TYPE_ENCODER:
        if (event->data.encoder.button) go_to_menu();
        break;
    default:
        break;
    }
}

static void get_scan_dashboard_callback(void **callback) {
    if (callback) *callback = scan_dashboard_view.input_callback;
}

View scan_dashboard_view = {
    .root = NULL,
    .create = scan_dashboard_create,
    .destroy = scan_dashboard_destroy,
    .name = "Live Scan",
    .get_hardwareinput_callback = get_scan_dashboard_callback,
    .input_callback = scan_dashboard_input,
};
