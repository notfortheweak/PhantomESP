// scan_dashboard_screen.c — see header.
#include "managers/views/scan_dashboard_screen.h"

#include "sdkconfig.h"
#include "lvgl.h"
#include "gui/scan_tile.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/scan_scheduler.h"
#include "managers/settings_manager.h"
#include "managers/views/main_menu_screen.h"

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
static lv_timer_t *s_timer = NULL;

static scan_tile_t *tile_wifi;
static scan_tile_t *tile_drones;
static scan_tile_t *tile_flock;
static scan_tile_t *tile_pineap;
#ifndef CONFIG_IDF_TARGET_ESP32S2
static scan_tile_t *tile_flippers;
static scan_tile_t *tile_airtags;
static scan_tile_t *tile_ble;
#endif

static scan_tile_t *add_tile(lv_obj_t *content, const char *label) {
    scan_tile_t *t = scan_tile_create(content, label);
    if (t) lv_obj_set_width(scan_tile_get_obj(t), LV_PCT(48));
    return t;
}

static void update_cb(lv_timer_t *timer) {
    (void)timer;

    int wifi = (int)ap_scan_get_count() + station_scan_get_count();
    scan_tile_set(tile_wifi, wifi, wifi > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);

    int drones = aerial_detector_get_device_count();
    scan_tile_set(tile_drones, drones, drones > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int flock = flock_detector_get_count();
    scan_tile_set(tile_flock, flock, flock > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int pineap = pineap_get_detected_count();
    scan_tile_set(tile_pineap, pineap, pineap > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

#ifndef CONFIG_IDF_TARGET_ESP32S2
    int flippers = flipper_scan_get_count();
    scan_tile_set(tile_flippers, flippers, flippers > 0 ? SCAN_SEV_THREAT : SCAN_SEV_IDLE);

    int airtags = airtag_scan_get_count();
    scan_tile_set(tile_airtags, airtags, airtags > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);

    int ble = ble_device_detect_get_count();
    scan_tile_set(tile_ble, ble, ble > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);
#endif
}

static void scan_dashboard_create(void) {
    if (scan_dashboard_view.root != NULL) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t bg = theme_palette_get_background(theme);

    display_manager_fill_screen(lv_color_hex(bg));
    s_root = gui_screen_create_root(NULL, "Live Scan", lv_color_hex(bg), LV_OPA_COVER);
    scan_dashboard_view.root = s_root;

    lv_obj_t *content = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(content, 4, 0);
    lv_obj_set_style_pad_row(content, 4, 0);
    lv_obj_set_style_pad_column(content, 4, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(content, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);
    lv_obj_add_flag(content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(content, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(content, LV_SCROLLBAR_MODE_AUTO);

    tile_wifi   = add_tile(content, "WiFi");
    tile_drones = add_tile(content, "Drones");
    tile_flock  = add_tile(content, "Flock Cam");
    tile_pineap = add_tile(content, "PineAP");
#ifndef CONFIG_IDF_TARGET_ESP32S2
    tile_flippers = add_tile(content, "Flippers");
    tile_airtags  = add_tile(content, "AirTags");
    tile_ble      = add_tile(content, "BLE");
#endif

    scan_scheduler_start();
    s_timer = lv_timer_create(update_cb, 500, NULL);
}

static void scan_dashboard_destroy(void) {
    if (s_timer) {
        lv_timer_del(s_timer);
        s_timer = NULL;
    }
    scan_scheduler_stop();

    scan_tile_destroy(tile_wifi);   tile_wifi = NULL;
    scan_tile_destroy(tile_drones); tile_drones = NULL;
    scan_tile_destroy(tile_flock);  tile_flock = NULL;
    scan_tile_destroy(tile_pineap); tile_pineap = NULL;
#ifndef CONFIG_IDF_TARGET_ESP32S2
    scan_tile_destroy(tile_flippers); tile_flippers = NULL;
    scan_tile_destroy(tile_airtags);  tile_airtags = NULL;
    scan_tile_destroy(tile_ble);      tile_ble = NULL;
#endif

    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = NULL;
    scan_dashboard_view.root = NULL;
}

// Any exit/select input leaves the monitor for the main menu (which stops the
// scheduler via destroy()). Drill-into-category detail is a later enhancement.
static void scan_dashboard_input(InputEvent *event) {
    if (!event) return;
    switch (event->type) {
    case INPUT_TYPE_TOUCH:
        if (event->data.touch_data.state == LV_INDEV_STATE_REL) {
            display_manager_switch_view(&main_menu_view);
        }
        break;
    case INPUT_TYPE_JOYSTICK:
        display_manager_switch_view(&main_menu_view);
        break;
    case INPUT_TYPE_KEYBOARD: {
        int key = event->data.key_value;
        if (key == LV_KEY_ESC || key == 27 || key == 29 || key == '`' ||
            key == 'q' || key == 'Q' || key == LV_KEY_ENTER || key == '\n' ||
            key == '\r' || key == ' ') {
            display_manager_switch_view(&main_menu_view);
        }
        break;
    }
    case INPUT_TYPE_ENCODER:
        if (event->data.encoder.button) {
            display_manager_switch_view(&main_menu_view);
        }
        break;
    case INPUT_TYPE_EXIT_BUTTON:
        display_manager_switch_view(&main_menu_view);
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
