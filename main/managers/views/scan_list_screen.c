// scan_list_screen.c — see header. Two views: a live per-category signal list
// and a single-signal detail screen. Touch arrives as InputEvents (the display
// manager does not wire an LVGL indev), so taps are hit-tested on release and
// drags are forwarded to the scroll container, mirroring main_menu_screen.
#include "managers/views/scan_list_screen.h"

#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

#include "gui/scan_report.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/views/scan_dashboard_screen.h"

#define SNAP_MAX 32

// ---- shared selection state ----
static scan_category_id_t s_category = SCAT_WIFI;
static scan_sig_t s_snap[SNAP_MAX];
static int s_snap_n = 0;
static int s_selected = 0;

void scan_list_set_category(scan_category_id_t category) {
    if (category >= 0 && category < SCAT_COUNT) s_category = category;
}

// ---- theme colors captured at create ----
static uint32_t c_bg, c_surface, c_text, c_dim, c_accent;

static void capture_theme(void) {
    uint8_t th = settings_get_menu_theme(&G_Settings);
    c_bg      = theme_palette_get_background(th);
    c_surface = theme_palette_get_surface_alt(th);
    c_text    = theme_palette_get_text(th);
    c_dim     = theme_palette_get_text_muted(th);
    c_accent  = theme_palette_get_accent(th);
}

static uint32_t rssi_color(int rssi) {
    if (rssi >= -60) return 0x00C853;   // strong  green
    if (rssi >= -80) return 0xFFAA00;   // medium  amber
    return 0xFF5252;                    // weak    red
}

// ============================================================================
// List view
// ============================================================================
static lv_obj_t *s_list_root = NULL;
static lv_obj_t *s_list_cont = NULL;
static lv_obj_t *s_back_btn = NULL;
static lv_obj_t *s_empty_lbl = NULL;
static lv_timer_t *s_list_timer = NULL;

static lv_obj_t *s_rows[SNAP_MAX];
static lv_obj_t *s_row_title[SNAP_MAX];
static lv_obj_t *s_row_sub[SNAP_MAX];
static lv_obj_t *s_row_rssi[SNAP_MAX];

// touch tracking
static bool s_touch_started = false, s_touch_dragged = false;
static int s_touch_sx, s_touch_sy, s_touch_lx, s_touch_ly;

static void snapshot_merge(void) {
    const scan_category_t *cat = scan_report_category(s_category);
    if (!cat || !cat->count || !cat->get) return;
    int n = cat->count();
    for (int i = 0; i < n; i++) {
        scan_sig_t sig;
        if (!cat->get(i, &sig)) continue;
        int found = -1;
        for (int j = 0; j < s_snap_n; j++) {
            if (strncmp(s_snap[j].title, sig.title, sizeof(sig.title)) == 0) { found = j; break; }
        }
        if (found < 0) {
            if (s_snap_n >= SNAP_MAX) continue;   // list full for this session
            found = s_snap_n++;
        }
        s_snap[found] = sig;   // refresh rssi/detail/sub in place
    }
}

static lv_obj_t *make_row(int idx) {
    lv_obj_t *row = lv_obj_create(s_list_cont);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, lv_color_hex(c_surface), 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_radius(row, 4, 0);
    lv_obj_set_style_pad_hor(row, 8, 0);
    lv_obj_set_style_pad_ver(row, 6, 0);
    lv_obj_clear_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    lv_obj_t *left = lv_obj_create(row);
    lv_obj_remove_style_all(left);
    lv_obj_set_flex_grow(left, 1);
    lv_obj_set_height(left, LV_SIZE_CONTENT);
    lv_obj_clear_flag(left, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);

    s_row_title[idx] = lv_label_create(left);
    lv_obj_set_style_text_color(s_row_title[idx], lv_color_hex(c_text), 0);
    lv_label_set_long_mode(s_row_title[idx], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_row_title[idx], LV_PCT(100));

    s_row_sub[idx] = lv_label_create(left);
    lv_obj_set_style_text_color(s_row_sub[idx], lv_color_hex(c_dim), 0);
    lv_label_set_long_mode(s_row_sub[idx], LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_row_sub[idx], LV_PCT(100));

    s_row_rssi[idx] = lv_label_create(row);
    lv_label_set_text(s_row_rssi[idx], "");
    return row;
}

static void list_refresh(lv_timer_t *t) {
    (void)t;
    snapshot_merge();

    for (int i = 0; i < s_snap_n; i++) {
        if (!s_rows[i]) s_rows[i] = make_row(i);
        lv_label_set_text(s_row_title[i], s_snap[i].title[0] ? s_snap[i].title : "?");
        lv_label_set_text(s_row_sub[i], s_snap[i].sub);
        if (s_snap[i].has_rssi) {
            lv_label_set_text_fmt(s_row_rssi[i], "%d", s_snap[i].rssi);
            lv_obj_set_style_text_color(s_row_rssi[i], lv_color_hex(rssi_color(s_snap[i].rssi)), 0);
        } else {
            lv_label_set_text(s_row_rssi[i], "");
        }
    }
    if (s_empty_lbl) {
        if (s_snap_n > 0) lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scan_list_create(void) {
    if (scan_list_view.root) return;
    capture_theme();
    s_snap_n = 0;
    memset(s_rows, 0, sizeof(s_rows));

    const scan_category_t *cat = scan_report_category(s_category);
    const char *title = cat && cat->name ? cat->name : "Signals";

    display_manager_fill_screen(lv_color_hex(c_bg));
    s_list_root = gui_screen_create_root(NULL, title, lv_color_hex(c_bg), LV_OPA_COVER);
    scan_list_view.root = s_list_root;

    s_list_cont = gui_screen_create_content(s_list_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(s_list_cont, 6, 0);
    lv_obj_set_style_pad_row(s_list_cont, 5, 0);
    lv_obj_set_flex_flow(s_list_cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(s_list_cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list_cont, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list_cont, LV_SCROLLBAR_MODE_AUTO);

    // Back button (first, always-at-top row).
    s_back_btn = lv_obj_create(s_list_cont);
    lv_obj_set_width(s_back_btn, LV_PCT(100));
    lv_obj_set_height(s_back_btn, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(s_back_btn, lv_color_hex(c_accent), 0);
    lv_obj_set_style_bg_opa(s_back_btn, LV_OPA_30, 0);
    lv_obj_set_style_border_width(s_back_btn, 0, 0);
    lv_obj_set_style_radius(s_back_btn, 4, 0);
    lv_obj_set_style_pad_ver(s_back_btn, 6, 0);
    lv_obj_set_style_pad_hor(s_back_btn, 8, 0);
    lv_obj_clear_flag(s_back_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *bl = lv_label_create(s_back_btn);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Back");
    lv_obj_set_style_text_color(bl, lv_color_hex(c_text), 0);

    s_empty_lbl = lv_label_create(s_list_cont);
    lv_label_set_text(s_empty_lbl, "Scanning...");
    lv_obj_set_style_text_color(s_empty_lbl, lv_color_hex(c_dim), 0);

    list_refresh(NULL);
    s_list_timer = lv_timer_create(list_refresh, 600, NULL);
}

static void scan_list_destroy(void) {
    if (s_list_timer) { lv_timer_del(s_list_timer); s_list_timer = NULL; }
    if (s_list_root && lv_obj_is_valid(s_list_root)) lv_obj_del(s_list_root);
    s_list_root = s_list_cont = s_back_btn = s_empty_lbl = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    scan_list_view.root = NULL;
    s_touch_started = false;
}

static bool point_in(lv_obj_t *obj, int x, int y) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static void open_selected_detail(int snap_index) {
    if (snap_index < 0 || snap_index >= s_snap_n) return;
    s_selected = snap_index;
    display_manager_switch_view(&scan_signal_view);
}

static void scan_list_input(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_started) {
                s_touch_started = true; s_touch_dragged = false;
                s_touch_sx = s_touch_lx = d->point.x;
                s_touch_sy = s_touch_ly = d->point.y;
            } else {
                int dy = d->point.y - s_touch_ly;
                s_touch_lx = d->point.x; s_touch_ly = d->point.y;
                if (abs(d->point.y - s_touch_sy) > 8 || abs(d->point.x - s_touch_sx) > 8)
                    s_touch_dragged = true;
                if (s_touch_dragged && s_list_cont && dy)
                    display_manager_queue_scroll(s_list_cont, dy);
            }
        } else if (d->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            if (s_touch_dragged) return;   // was a scroll, not a tap
            int x = d->point.x, y = d->point.y;
            if (point_in(s_back_btn, x, y)) { display_manager_switch_view(&scan_dashboard_view); return; }
            for (int i = 0; i < s_snap_n; i++) {
                if (point_in(s_rows[i], x, y)) { open_selected_detail(i); return; }
            }
        }
        return;
    }

    // Hardware back → dashboard.
    switch (event->type) {
    case INPUT_TYPE_EXIT_BUTTON:
        display_manager_switch_view(&scan_dashboard_view);
        break;
    case INPUT_TYPE_KEYBOARD: {
        int k = event->data.key_value;
        if (k == LV_KEY_ESC || k == 27 || k == '`' || k == 'q' || k == 'Q' ||
            k == LV_KEY_LEFT)
            display_manager_switch_view(&scan_dashboard_view);
        break;
    }
    case INPUT_TYPE_ENCODER:
        if (event->data.encoder.button) display_manager_switch_view(&scan_dashboard_view);
        break;
    default:
        break;
    }
}

static void scan_list_get_cb(void **cb) { if (cb) *cb = scan_list_view.input_callback; }

View scan_list_view = {
    .root = NULL,
    .create = scan_list_create,
    .destroy = scan_list_destroy,
    .name = "Scan List",
    .get_hardwareinput_callback = scan_list_get_cb,
    .input_callback = scan_list_input,
};

// ============================================================================
// Signal detail view
// ============================================================================
static lv_obj_t *s_sig_root = NULL;
static bool s_sig_touch_started = false, s_sig_touch_dragged = false;
static int s_sig_sx, s_sig_sy;

static void scan_signal_create(void) {
    if (scan_signal_view.root) return;
    capture_theme();

    const char *title = (s_selected >= 0 && s_selected < s_snap_n)
                        ? s_snap[s_selected].title : "Signal";

    display_manager_fill_screen(lv_color_hex(c_bg));
    s_sig_root = gui_screen_create_root(NULL, title, lv_color_hex(c_bg), LV_OPA_COVER);
    scan_signal_view.root = s_sig_root;

    lv_obj_t *cont = gui_screen_create_content(s_sig_root, GUI_STATUS_BAR_HEIGHT);
    lv_obj_set_style_pad_all(cont, 10, 0);
    lv_obj_set_style_pad_row(cont, 10, 0);
    lv_obj_set_flex_flow(cont, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(cont, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(cont, LV_DIR_VER);

    lv_obj_t *card = lv_obj_create(cont);
    lv_obj_set_width(card, LV_PCT(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, lv_color_hex(c_surface), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(c_text), 0);
    lv_label_set_text(body, (s_selected >= 0 && s_selected < s_snap_n)
                            ? s_snap[s_selected].detail : "No data.");

    lv_obj_t *hint = lv_label_create(cont);
    lv_label_set_text(hint, LV_SYMBOL_LEFT "  Tap or Back to return");
    lv_obj_set_style_text_color(hint, lv_color_hex(c_dim), 0);
}

static void scan_signal_destroy(void) {
    if (s_sig_root && lv_obj_is_valid(s_sig_root)) lv_obj_del(s_sig_root);
    s_sig_root = NULL;
    scan_signal_view.root = NULL;
    s_sig_touch_started = false;
}

static void scan_signal_input(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_sig_touch_started) {
                s_sig_touch_started = true; s_sig_touch_dragged = false;
                s_sig_sx = d->point.x; s_sig_sy = d->point.y;
            } else if (abs(d->point.y - s_sig_sy) > 8 || abs(d->point.x - s_sig_sx) > 8) {
                s_sig_touch_dragged = true;
            }
        } else if (d->state == LV_INDEV_STATE_REL && s_sig_touch_started) {
            s_sig_touch_started = false;
            if (!s_sig_touch_dragged) display_manager_switch_view(&scan_list_view);
        }
        return;
    }
    display_manager_switch_view(&scan_list_view);   // any hardware input → back
}

static void scan_signal_get_cb(void **cb) { if (cb) *cb = scan_signal_view.input_callback; }

View scan_signal_view = {
    .root = NULL,
    .create = scan_signal_create,
    .destroy = scan_signal_destroy,
    .name = "Signal",
    .get_hardwareinput_callback = scan_signal_get_cb,
    .input_callback = scan_signal_input,
};
