// aerial_detail_screen.c — dedicated drone readout. Reads the live AerialDevice
// (by MAC) and shows aircraft telemetry plus, prominently, the OPERATOR/takeoff
// location decoded from DJI DroneID. LVGL-pool-safe (a handful of labels created
// on entry, freed on back), modelled on scan_signal_view.
#include "managers/views/aerial_detail_screen.h"

#include "lvgl.h"
#include <string.h>
#include <stdio.h>
#include <math.h>

#include "gui/screen_layout.h"
#include "gui/theme_palette_api.h"
#include "managers/display_manager.h"
#include "managers/settings_manager.h"
#include "managers/aerial_detector_manager.h"
#include "managers/gps_manager.h"
#include "managers/views/scan_list_screen.h"

static char s_mac[20];
void aerial_detail_set_mac(const char *mac) {
    snprintf(s_mac, sizeof(s_mac), "%s", mac ? mac : "");
}

static uint32_t c_bg, c_surface, c_text, c_dim, c_accent;
static void capture_theme(void) {
    uint8_t th = settings_get_menu_theme(&G_Settings);
    c_bg      = theme_palette_get_background(th);
    c_surface = theme_palette_get_surface_alt(th);
    c_text    = theme_palette_get_text(th);
    c_dim     = theme_palette_get_text_muted(th);
    c_accent  = theme_palette_get_accent(th);
}

static lv_obj_t *s_root = NULL, *s_body = NULL;
static lv_timer_t *s_timer = NULL;
static bool s_touch_started, s_touch_dragged;
static int s_sx, s_sy;

// Great-circle distance (m) and initial bearing (deg) from us to the operator.
static void geo_to(double lat1, double lon1, double lat2, double lon2,
                   double *dist_m, double *bearing_deg) {
    double r = 6371000.0, d2r = M_PI / 180.0;
    double p1 = lat1 * d2r, p2 = lat2 * d2r;
    double dp = (lat2 - lat1) * d2r, dl = (lon2 - lon1) * d2r;
    double a = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2);
    *dist_m = r * 2 * atan2(sqrt(a), sqrt(1 - a));
    double y = sin(dl) * cos(p2);
    double x = cos(p1) * sin(p2) - sin(p1) * cos(p2) * cos(dl);
    double b = atan2(y, x) / d2r;
    *bearing_deg = b < 0 ? b + 360.0 : b;
}

static const char *compass8(double bearing) {
    static const char *pts[] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    return pts[((int)((bearing + 22.5) / 45.0)) & 7];
}

static void render(void) {
    if (!s_body || !lv_obj_is_valid(s_body)) return;
    AerialDevice dev;
    AerialDevice *live = aerial_detector_find_device_by_mac(s_mac);
    if (!live) {
        lv_label_set_text(s_body, "Signal lost.\nThe drone is no longer being heard.");
        return;
    }
    dev = *live;   // snapshot; the sniffer keeps updating the live copy

    char buf[512];
    int p = 0;
    p += snprintf(buf + p, sizeof(buf) - p, "%s\n%s\n%s   %d dBm   ch%d\n\n",
                  dev.vendor[0] ? dev.vendor : "Drone",
                  dev.device_id[0] ? dev.device_id : "(no serial)",
                  aerial_detector_get_type_string(dev.type), dev.rssi, dev.channel);

    p += snprintf(buf + p, sizeof(buf) - p, "#8AB4FF AIRCRAFT#\n");
    if (dev.has_location) {
        p += snprintf(buf + p, sizeof(buf) - p, "Lat %.6f\nLon %.6f\n",
                      dev.latitude, dev.longitude);
        p += snprintf(buf + p, sizeof(buf) - p, "Alt %.0f m   AGL %.0f m\n",
                      dev.altitude, dev.height_agl);
        if (dev.speed_horizontal >= 0.0f)
            p += snprintf(buf + p, sizeof(buf) - p, "Hdg %.0f\xC2\xB0   %.1f m/s\n",
                          dev.direction, dev.speed_horizontal);
        else
            p += snprintf(buf + p, sizeof(buf) - p, "Hdg %.0f\xC2\xB0\n", dev.direction);
    } else {
        p += snprintf(buf + p, sizeof(buf) - p, "position not broadcast\n");
    }

    p += snprintf(buf + p, sizeof(buf) - p, "\n#FF6B6B OPERATOR#\n");
    if (dev.has_operator_location) {
        p += snprintf(buf + p, sizeof(buf) - p, "Lat %.6f\nLon %.6f\n",
                      dev.operator_latitude, dev.operator_longitude);
        // If we have our own fix, point toward the operator.
        gps_t g = {0};
        bool peer = false;
        if (gps_manager_get_active_gps_snapshot(&g, &peer) && g.valid) {
            double dist, brg;
            geo_to(g.latitude, g.longitude, dev.operator_latitude,
                   dev.operator_longitude, &dist, &brg);
            if (dist < 1000.0)
                p += snprintf(buf + p, sizeof(buf) - p, "%.0f m  %s (%.0f\xC2\xB0)\n",
                              dist, compass8(brg), brg);
            else
                p += snprintf(buf + p, sizeof(buf) - p, "%.2f km  %s (%.0f\xC2\xB0)\n",
                              dist / 1000.0, compass8(brg), brg);
        }
    } else {
        p += snprintf(buf + p, sizeof(buf) - p,
                      "not broadcast / encrypted\n(only sent by armed drones)");
    }
    lv_label_set_text(s_body, buf);
}

static void tick(lv_timer_t *t) { (void)t; render(); }

static void aerial_detail_create(void) {
    if (aerial_detail_view.root) return;
    capture_theme();

    display_manager_fill_screen(lv_color_hex(c_bg));
    s_root = gui_screen_create_root(NULL, "Drone", lv_color_hex(c_bg), LV_OPA_COVER);
    aerial_detail_view.root = s_root;

    lv_obj_t *cont = gui_screen_create_content(s_root, GUI_STATUS_BAR_HEIGHT);
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
    lv_obj_set_style_border_color(card, lv_color_hex(0xFF6B6B), 0);
    lv_obj_set_style_radius(card, 6, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_clear_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    s_body = lv_label_create(card);
    lv_label_set_recolor(s_body, true);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_body, LV_PCT(100));
    lv_obj_set_style_text_color(s_body, lv_color_hex(c_text), 0);
    lv_label_set_text(s_body, "Acquiring telemetry...");

    lv_obj_t *hint = lv_label_create(cont);
    lv_label_set_text(hint, LV_SYMBOL_LEFT "  Tap or Back to return");
    lv_obj_set_style_text_color(hint, lv_color_hex(c_dim), 0);

    render();
    s_timer = lv_timer_create(tick, 600, NULL);
}

static void aerial_detail_destroy(void) {
    if (s_timer) { lv_timer_del(s_timer); s_timer = NULL; }
    if (s_root && lv_obj_is_valid(s_root)) lv_obj_del(s_root);
    s_root = NULL; s_body = NULL;
    aerial_detail_view.root = NULL;
    s_touch_started = false;
}

static void aerial_detail_input(InputEvent *event) {
    if (!event) return;
    if (event->type == INPUT_TYPE_TOUCH) {
        lv_indev_data_t *d = &event->data.touch_data;
        if (d->state == LV_INDEV_STATE_PR) {
            if (!s_touch_started) {
                s_touch_started = true; s_touch_dragged = false;
                s_sx = d->point.x; s_sy = d->point.y;
            } else if (abs(d->point.y - s_sy) > 16 || abs(d->point.x - s_sx) > 16) {
                s_touch_dragged = true;
            }
        } else if (d->state == LV_INDEV_STATE_REL && s_touch_started) {
            s_touch_started = false;
            if (!s_touch_dragged) display_manager_switch_view(&scan_list_view);
        }
        return;
    }
    display_manager_switch_view(&scan_list_view);
}

static void aerial_detail_get_cb(void **cb) { if (cb) *cb = aerial_detail_view.input_callback; }

View aerial_detail_view = {
    .root = NULL,
    .create = aerial_detail_create,
    .destroy = aerial_detail_destroy,
    .name = "Drone Detail",
    .get_hardwareinput_callback = aerial_detail_get_cb,
    .input_callback = aerial_detail_input,
};
