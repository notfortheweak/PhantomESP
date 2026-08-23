// scan_dashboard_screen.c — see header. Live threat dashboard: a tappable tile
// per category. Tapping a tile drills into its live signal list
// (scan_list_view); the "Menu" button (or hardware back) returns to the main
// menu. The scan scheduler keeps running while drilling into sub-views — it is
// stopped only when leaving Live Scan for the main menu.
#include "managers/views/scan_dashboard_screen.h"

#include "sdkconfig.h"
#include "lvgl.h"
#include <stdlib.h>
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

static scan_tile_t *tile_for(scan_category_id_t cat) {
    for (int i = 0; i < s_ntiles; i++) if (s_tiles[i].cat == cat) return s_tiles[i].tile;
    return NULL;
}

static void add_tile(lv_obj_t *content, const char *label, scan_category_id_t cat) {
    scan_tile_t *t = scan_tile_create(content, label);
    if (!t) return;
    lv_obj_set_width(scan_tile_get_obj(t), LV_PCT(48));
    if (s_ntiles < MAX_TILES) { s_tiles[s_ntiles].tile = t; s_tiles[s_ntiles].cat = cat; s_ntiles++; }
}

static void set_tile(scan_category_id_t cat, int count, scan_severity_t sev) {
    scan_tile_set(tile_for(cat), count, sev);
}

static void update_cb(lv_timer_t *timer) {
    (void)timer;
    int wifi = (int)ap_scan_get_count() + station_scan_get_count();
    set_tile(SCAT_WIFI, wifi, wifi > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);

    int drones = aerial_detector_get_device_count();
    set_tile(SCAT_DRONES, drones, drones > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int flock = flock_detector_get_count();
    set_tile(SCAT_FLOCK, flock, flock > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int pineap = pineap_get_detected_count();
    set_tile(SCAT_PINEAP, pineap, pineap > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

#ifndef CONFIG_IDF_TARGET_ESP32S2
    int flippers = flipper_scan_get_count();
    set_tile(SCAT_FLIPPERS, flippers, flippers > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int airtags = airtag_scan_get_count();
    set_tile(SCAT_AIRTAGS, airtags, airtags > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);

    int ble = ble_device_detect_get_count();
    set_tile(SCAT_BLE, ble, ble > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);
#endif
}

static void scan_dashboard_create(void) {
    if (scan_dashboard_view.root != NULL) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t bg = theme_palette_get_background(theme);
    uint32_t accent = theme_palette_get_accent(theme);
    uint32_t text = theme_palette_get_text(theme);

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

    add_tile(content, "WiFi", SCAT_WIFI);
    add_tile(content, "Drones", SCAT_DRONES);
    add_tile(content, "Flock Cam", SCAT_FLOCK);
    add_tile(content, "PineAP", SCAT_PINEAP);
#ifndef CONFIG_IDF_TARGET_ESP32S2
    add_tile(content, "Flippers", SCAT_FLIPPERS);
    add_tile(content, "AirTags", SCAT_AIRTAGS);
    add_tile(content, "BLE", SCAT_BLE);
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
    scan_scheduler_stop();
    display_manager_switch_view(&main_menu_view);
}

static void open_category(scan_category_id_t cat) {
    scan_list_set_category(cat);
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
