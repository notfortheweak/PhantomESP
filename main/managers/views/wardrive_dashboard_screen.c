// wardrive_dashboard_screen.c — Live-Scan-style wardriving dashboard. Owns the
// wardriving engine + CSV for the session, feeds nothing itself (the CSV logging
// path feeds wardrive_report on the scan thread); tiles + GPS header just read
// wardrive_report + a GPS snapshot. Tap the networks tile to drill into the list.
#include "managers/views/wardrive_dashboard_screen.h"

#include "sdkconfig.h"
#include "lvgl.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "gui/scan_tile.h"
#include "gui/wardrive_report.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/wardrive_list_screen.h"

#include "managers/gps_manager.h"
#include "vendor/GPS/gps_logger.h"
#include "core/callbacks.h"           // start/stop_wardriving, wardriving_scan_callback, ble_wardriving_callback
#include "managers/wifi_manager.h"
#ifndef CONFIG_IDF_TARGET_ESP32S2
#include "managers/ble_manager.h"
#endif

static bool s_ble_mode = false;   // set before switching in; false = WiFi mode

static lv_obj_t *s_root = NULL, *s_content = NULL, *s_back_btn = NULL, *s_gps_hdr = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_owns_gps = false;
static bool s_owns_csv = false;

static uint32_t s_saved_timeout = 0;
static bool s_screen_forced = false;

typedef enum { WD_TILE_NET, WD_TILE_LOGGED, WD_TILE_SPEED, WD_TILE_GPS } wd_tile_id_t;
#define MAX_TILES 6
static struct { scan_tile_t *tile; wd_tile_id_t id; } s_tiles[MAX_TILES];
static int s_ntiles = 0;

static bool s_touch_started = false, s_touch_dragged = false;
static int s_sx, s_sy, s_lx, s_ly;

void wardrive_dashboard_set_ble_mode(bool enabled) { s_ble_mode = enabled; }

static void add_tile(lv_obj_t *content, const char *label, const char *icon,
                     wd_tile_id_t id) {
    scan_tile_t *t = scan_tile_create(content, label, icon);
    if (!t) return;
    lv_obj_set_width(scan_tile_get_obj(t), LV_PCT(48));
    if (s_ntiles < MAX_TILES) { s_tiles[s_ntiles].tile = t; s_tiles[s_ntiles].id = id; s_ntiles++; }
}

// ---- engine lifecycle (mirrors wardriving_screen.c, standalone only) ----
static void wardrive_engine_start(void) {
    if (!g_gpsManager.isinitilized) {
        gps_manager_init(&g_gpsManager);
        s_owns_gps = true;
    }
    if (s_ble_mode) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
        ble_wardriving_reset_unique_device_count();
        if (csv_file_open("ble_wardriving") == ESP_OK) {
            s_owns_csv = true;
            ble_start_scanning();
            ble_register_handler(ble_wardriving_callback);
        }
#endif
    } else {
        if (csv_file_open("wardriving") == ESP_OK) {
            s_owns_csv = true;
            wifi_manager_start_monitor_mode(wardriving_scan_callback);
            if (!start_wardriving()) {
                wifi_manager_stop_monitor_mode();
            }
        }
    }
}

static void wardrive_engine_stop(void) {
    if (s_owns_csv) {
        if (s_ble_mode) {
#ifndef CONFIG_IDF_TARGET_ESP32S2
            ble_unregister_handler(ble_wardriving_callback);
            ble_stop();
#endif
        } else {
            stop_wardriving();
            wifi_manager_stop_monitor_mode();
        }
        if (csv_buffer_has_pending_data()) csv_flush_buffer_to_file();
        csv_file_close();
        s_owns_csv = false;
    }
    if (s_owns_gps) {
        gps_manager_deinit(&g_gpsManager);
        s_owns_gps = false;
    }
}

// ---- GPS header ----
static void gps_header_text(char *buf, size_t n) {
    gps_t g = {0};
    bool peer = false;
    bool have = gps_manager_get_active_gps_snapshot(&g, &peer);
    if (!have || !g.valid) { snprintf(buf, n, "GPS: acquiring fix..."); return; }
    const char *fix = (g.fix_mode == GPS_MODE_3D) ? "3D" :
                      (g.fix_mode == GPS_MODE_2D) ? "2D" : "Fix";
    snprintf(buf, n, "%s  %d/%d sats  %.5f, %.5f%s", fix, g.sats_in_use, g.sats_in_view,
             g.latitude, g.longitude, peer ? "  (peer)" : "");
}

static void update_cb(lv_timer_t *timer) {
    (void)timer;
    wd_kind_t kind = s_ble_mode ? WD_KIND_BLE : WD_KIND_WIFI;

    gps_t g = {0};
    bool peer = false;
    bool have_fix = gps_manager_get_active_gps_snapshot(&g, &peer) && g.valid;

    char primary[24];
    for (int i = 0; i < s_ntiles; i++) {
        switch (s_tiles[i].id) {
        case WD_TILE_NET: {
            int distinct = wardrive_report_count(kind);
            int active = wardrive_report_active_count(kind);
            snprintf(primary, sizeof(primary), "%d", distinct);
            scan_tile_set(s_tiles[i].tile, primary, 0,
                          active > 0 ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);
            break;
        }
        case WD_TILE_LOGGED:
            snprintf(primary, sizeof(primary), "%lu",
                     (unsigned long)wardrive_report_logged_total());
            scan_tile_set(s_tiles[i].tile, primary, 0, SCAN_SEV_IDLE);
            break;
        case WD_TILE_SPEED:
            snprintf(primary, sizeof(primary), "%.0f", have_fix ? g.speed * 3.6f : 0.0f);
            scan_tile_set(s_tiles[i].tile, primary, 0, SCAN_SEV_IDLE);
            break;
        case WD_TILE_GPS: {
            const char *fix = !have_fix ? "--" :
                              (g.fix_mode == GPS_MODE_3D) ? "3D" :
                              (g.fix_mode == GPS_MODE_2D) ? "2D" : "Fix";
            snprintf(primary, sizeof(primary), "%s", fix);
            scan_tile_set(s_tiles[i].tile, primary, 0,
                          have_fix ? SCAN_SEV_PRESENT : SCAN_SEV_IDLE);
            break;
        }
        }
    }

    if (s_gps_hdr && lv_obj_is_valid(s_gps_hdr)) {
        char hdr[64];
        gps_header_text(hdr, sizeof(hdr));
        lv_label_set_text(s_gps_hdr, hdr);
    }
}

static void wardrive_dashboard_create(void) {
    if (wardrive_dashboard_view.root != NULL) return;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    uint32_t bg = theme_palette_get_background(theme);
    uint32_t accent = theme_palette_get_accent(theme);
    uint32_t text = theme_palette_get_text(theme);
    uint32_t dim = theme_palette_get_text_muted(theme);

    // Keep the screen on for the whole wardriving session (restored on exit).
    if (!s_screen_forced) {
        s_saved_timeout = G_Settings.display_timeout_ms;
        G_Settings.display_timeout_ms = UINT32_MAX;
        s_screen_forced = true;
        wardrive_report_alloc();
        wardrive_report_reset();
        wardrive_engine_start();
    }

    s_ntiles = 0;
    display_manager_fill_screen(lv_color_hex(bg));
    s_root = gui_screen_create_root(NULL, s_ble_mode ? "BLE Wardriving" : "Wardriving",
                                    lv_color_hex(bg), LV_OPA_COVER);
    wardrive_dashboard_view.root = s_root;

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

    // Full-width GPS status header.
    s_gps_hdr = lv_label_create(content);
    lv_obj_set_width(s_gps_hdr, LV_PCT(100));
    lv_label_set_long_mode(s_gps_hdr, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(s_gps_hdr, lv_color_hex(dim), 0);
    lv_label_set_text(s_gps_hdr, "GPS: acquiring fix...");

    add_tile(content, s_ble_mode ? "BLE Devs" : "WiFi APs",
             s_ble_mode ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_WIFI, WD_TILE_NET);
    add_tile(content, "Logged", LV_SYMBOL_SAVE, WD_TILE_LOGGED);
    add_tile(content, "Speed km/h", LV_SYMBOL_UP, WD_TILE_SPEED);
    add_tile(content, "GPS", LV_SYMBOL_GPS, WD_TILE_GPS);

    update_cb(NULL);
    s_timer = lv_timer_create(update_cb, 500, NULL);
}

static void wardrive_dashboard_destroy(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    for (int i = 0; i < s_ntiles; i++) scan_tile_destroy(s_tiles[i].tile);
    s_ntiles = 0;
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = s_content = s_back_btn = s_gps_hdr = NULL;
    wardrive_dashboard_view.root = NULL;
    s_touch_started = false;
    // NOTE: engine + buffer stay alive while drilling into the list/detail; they
    // are torn down only when leaving wardriving for the menu (go_to_menu()).
}

static void go_to_menu(void) {
    if (s_screen_forced) {
        G_Settings.display_timeout_ms = s_saved_timeout;
        s_screen_forced = false;
    }
    wardrive_engine_stop();
    wardrive_report_free();
    display_manager_switch_view(&main_menu_view);
}

static bool point_in(lv_obj_t *obj, int x, int y) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static void open_list(void) {
    wardrive_list_set_kind(s_ble_mode ? WD_KIND_BLE : WD_KIND_WIFI);
    display_manager_switch_view(&wardrive_list_view);   // engine keeps running
}

static void wardrive_dashboard_input(InputEvent *event) {
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
                if (s_tiles[i].id == WD_TILE_NET &&
                    point_in(scan_tile_get_obj(s_tiles[i].tile), x, y)) {
                    open_list();
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

static void get_wardrive_dashboard_callback(void **callback) {
    if (callback) *callback = wardrive_dashboard_view.input_callback;
}

View wardrive_dashboard_view = {
    .root = NULL,
    .create = wardrive_dashboard_create,
    .destroy = wardrive_dashboard_destroy,
    .name = "Wardrive",
    .get_hardwareinput_callback = get_wardrive_dashboard_callback,
    .input_callback = wardrive_dashboard_input,
};
