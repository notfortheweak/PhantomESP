// scan_list_screen.c — Live Scan drill-down. The list reads only the session
// accumulator (scan_report_*), which the scheduler fills — so the LVGL task
// never touches live engine data (no races). Rows are colored by kind
// (AP=blue, Station=red) and dimmed once a signal is no longer present.
#include "managers/views/scan_list_screen.h"

#include "lvgl.h"
#include <stdlib.h>
#include <string.h>

#include "gui/scan_report.h"
#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/scan_scheduler.h"
#include "managers/settings_manager.h"
#include "managers/views/scan_dashboard_screen.h"

#define MAX_ROWS 24

static scan_category_id_t s_category = SCAT_WIFI;
static scan_sig_t s_selected_sig;   // copied on tap for the detail view
static bool s_selected_active;

void scan_list_set_category(scan_category_id_t category) {
    if (category >= 0 && category < SCAT_COUNT) s_category = category;
}

// theme colors captured at create
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
    if (rssi >= -60) return 0x00C853;
    if (rssi >= -80) return 0xFFAA00;
    return 0xFF5252;
}
static uint32_t kind_color(scan_kind_t k) {
    switch (k) {
    case SKIND_AP:      return SCAN_COLOR_AP;
    case SKIND_STATION: return SCAN_COLOR_STATION;
    default:            return c_text;
    }
}

// ============================================================================
// List view
// ============================================================================
static lv_obj_t *s_list_root, *s_list_cont, *s_back_btn, *s_hdr, *s_empty_lbl;
static lv_timer_t *s_list_timer;

static lv_obj_t *s_rows[MAX_ROWS], *s_row_title[MAX_ROWS], *s_row_sub[MAX_ROWS], *s_row_rssi[MAX_ROWS];
static scan_sig_t s_rowsig[MAX_ROWS];
static bool s_rowactive[MAX_ROWS];
static int s_nrows;

static bool s_touch_started, s_touch_dragged;
static int s_sx, s_sy, s_lx, s_ly;

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
    int total = scan_report_total_count(s_category);
    if (total > MAX_ROWS) total = MAX_ROWS;
    s_nrows = 0;
    for (int i = 0; i < total; i++) {
        if (!scan_report_seen_get(s_category, i, &s_rowsig[i], &s_rowactive[i])) break;
        s_nrows++;
    }

    for (int i = 0; i < s_nrows; i++) {
        if (!s_rows[i]) s_rows[i] = make_row(i);
        bool act = s_rowactive[i];
        lv_obj_set_style_bg_opa(s_rows[i], act ? LV_OPA_COVER : LV_OPA_40, 0);
        lv_label_set_text(s_row_title[i], s_rowsig[i].title[0] ? s_rowsig[i].title : "?");
        lv_obj_set_style_text_color(s_row_title[i],
                                    lv_color_hex(act ? kind_color(s_rowsig[i].kind) : c_dim), 0);
        lv_label_set_text(s_row_sub[i], s_rowsig[i].sub);
        if (s_rowsig[i].has_rssi) {
            lv_label_set_text_fmt(s_row_rssi[i], "%d", s_rowsig[i].rssi);
            lv_obj_set_style_text_color(s_row_rssi[i],
                                        lv_color_hex(act ? rssi_color(s_rowsig[i].rssi) : c_dim), 0);
        } else {
            lv_label_set_text(s_row_rssi[i], "");
        }
    }

    int active = scan_report_active_count(s_category);
    lv_label_set_text_fmt(s_hdr, "Active: %d   #B388FF Total: %d#", active, total);
    if (s_empty_lbl) {
        if (s_nrows > 0) lv_obj_add_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_clear_flag(s_empty_lbl, LV_OBJ_FLAG_HIDDEN);
    }
}

static void scan_list_create(void) {
    if (scan_list_view.root) return;
    capture_theme();
    memset(s_rows, 0, sizeof(s_rows));
    s_nrows = 0;

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

    s_hdr = lv_label_create(s_list_cont);
    lv_label_set_recolor(s_hdr, true);
    lv_obj_set_style_text_color(s_hdr, lv_color_hex(c_text), 0);
    lv_label_set_text(s_hdr, "Active: 0   #B388FF Total: 0#");

    s_empty_lbl = lv_label_create(s_list_cont);
    lv_label_set_text(s_empty_lbl, "Scanning...");
    lv_obj_set_style_text_color(s_empty_lbl, lv_color_hex(c_dim), 0);

    list_refresh(NULL);
    s_list_timer = lv_timer_create(list_refresh, 600, NULL);
}

static void scan_list_destroy(void) {
    if (s_list_timer) { lv_timer_del(s_list_timer); s_list_timer = NULL; }
    if (s_list_root && lv_obj_is_valid(s_list_root)) lv_obj_del(s_list_root);
    s_list_root = s_list_cont = s_back_btn = s_hdr = s_empty_lbl = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    scan_list_view.root = NULL;
    s_touch_started = false;
}

static bool point_in(lv_obj_t *obj, int x, int y) {
    if (!obj || !lv_obj_is_valid(obj)) return false;
    lv_area_t a; lv_obj_get_coords(obj, &a);
    return x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2;
}

static void scan_list_input(InputEvent *event) {
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
                if (abs(d->point.y - s_sy) > 8 || abs(d->point.x - s_sx) > 8) s_touch_dragged = true;
                if (s_touch_dragged && s_list_cont && dy) display_manager_queue_scroll(s_list_cont, dy);
            }
        } else if (d->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            if (s_touch_dragged) return;
            int x = d->point.x, y = d->point.y;
            if (point_in(s_back_btn, x, y)) { display_manager_switch_view(&scan_dashboard_view); return; }
            for (int i = 0; i < s_nrows; i++) {
                if (point_in(s_rows[i], x, y)) {
                    s_selected_sig = s_rowsig[i];
                    s_selected_active = s_rowactive[i];
                    display_manager_switch_view(&scan_signal_view);
                    return;
                }
            }
        }
        return;
    }

    switch (event->type) {
    case INPUT_TYPE_EXIT_BUTTON:
        display_manager_switch_view(&scan_dashboard_view);
        break;
    case INPUT_TYPE_KEYBOARD: {
        int k = event->data.key_value;
        if (k == LV_KEY_ESC || k == 27 || k == '`' || k == 'q' || k == 'Q' || k == LV_KEY_LEFT)
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
static bool s_sig_touch_started, s_sig_touch_dragged;
static int s_sig_sx, s_sig_sy;

static void scan_signal_create(void) {
    if (scan_signal_view.root) return;
    capture_theme();

    display_manager_fill_screen(lv_color_hex(c_bg));
    s_sig_root = gui_screen_create_root(NULL, s_selected_sig.title[0] ? s_selected_sig.title : "Signal",
                                        lv_color_hex(c_bg), LV_OPA_COVER);
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
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_color(card, lv_color_hex(kind_color(s_selected_sig.kind)), 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *body = lv_label_create(card);
    lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(body, LV_PCT(100));
    lv_obj_set_style_text_color(body, lv_color_hex(c_text), 0);

    char buf[288];
    int p = 0;
    p += snprintf(buf + p, sizeof(buf) - p, "%s\n",
                  s_selected_sig.title[0] ? s_selected_sig.title : "Signal");
    if (s_selected_sig.sub[0])
        p += snprintf(buf + p, sizeof(buf) - p, "%s\n", s_selected_sig.sub);
    if (s_selected_sig.addr[0] && strcmp(s_selected_sig.addr, s_selected_sig.title) != 0) {
        const char *lbl = (s_selected_sig.kind == SKIND_STATION) ? "Assoc AP BSSID" : "Address";
        p += snprintf(buf + p, sizeof(buf) - p, "%s: %s\n", lbl, s_selected_sig.addr);
    }
    if (s_selected_sig.has_rssi)
        p += snprintf(buf + p, sizeof(buf) - p, "RSSI: %d dBm\n", s_selected_sig.rssi);
    p += snprintf(buf + p, sizeof(buf) - p, "%s",
                  s_selected_active ? "Status: active" : "Status: no longer present");
    lv_label_set_text(body, buf);

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
    display_manager_switch_view(&scan_list_view);
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
