// scan_tile.c — see header.
#include "gui/scan_tile.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"

#include <stdlib.h>

#define SCAN_TILE_BAR_MAX 20   // count magnitude at which the level bar is full

struct scan_tile_t {
    lv_obj_t *card;
    lv_obj_t *count_label;
    lv_obj_t *bar;        // level-bar track (plain lv_obj; lv_bar isn't built on
                          // every board's LVGL config, so we roll our own)
    lv_obj_t *bar_fill;   // indicator child, width set as a percentage of track
    int last_count;
    scan_severity_t last_sev;
    // palette (captured at create so update() stays theme-consistent)
    uint32_t surface;
    uint32_t text;
    uint32_t dim;
    uint32_t sev_present;
    uint32_t sev_threat;
};

static uint32_t sev_color(const scan_tile_t *t, scan_severity_t sev) {
    switch (sev) {
    case SCAN_SEV_THREAT:  return t->sev_threat;
    case SCAN_SEV_PRESENT: return t->sev_present;
    default:               return t->dim;
    }
}

scan_tile_t *scan_tile_create(lv_obj_t *parent, const char *label) {
    if (!parent) return NULL;
    scan_tile_t *t = calloc(1, sizeof(scan_tile_t));
    if (!t) return NULL;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    t->surface     = theme_palette_get_surface_alt(theme);
    t->text        = theme_palette_get_text(theme);
    t->dim         = theme_palette_get_text_muted(theme);
    t->sev_present = 0xFFAA00;  // amber
    t->sev_threat  = 0xFF4444;  // red
    t->last_count  = -1;
    t->last_sev    = (scan_severity_t)-1;

    t->card = lv_obj_create(parent);
    lv_obj_set_size(t->card, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t->card, lv_color_hex(t->surface), 0);
    lv_obj_set_style_bg_opa(t->card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(t->dim), 0);
    lv_obj_set_style_border_width(t->card, 2, 0);
    lv_obj_set_style_border_side(t->card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_radius(t->card, 4, 0);
    lv_obj_set_style_pad_all(t->card, 6, 0);
    lv_obj_set_style_pad_row(t->card, 2, 0);
    lv_obj_clear_flag(t->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(t->card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(t->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(t->card);
    lv_label_set_text(title, label ? label : "");
    lv_obj_set_style_text_color(title, lv_color_hex(t->dim), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));

    t->count_label = lv_label_create(t->card);
    lv_label_set_text(t->count_label, "0");
    lv_obj_set_style_text_color(t->count_label, lv_color_hex(t->dim), 0);
    lv_obj_set_style_text_font(t->count_label, &lv_font_montserrat_24, 0);

    t->bar = lv_obj_create(t->card);
    lv_obj_set_size(t->bar, LV_PCT(100), 4);
    lv_obj_set_style_bg_color(t->bar, lv_color_hex(t->surface), 0);
    lv_obj_set_style_bg_opa(t->bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t->bar, 0, 0);
    lv_obj_set_style_radius(t->bar, 0, 0);
    lv_obj_set_style_pad_all(t->bar, 0, 0);
    lv_obj_clear_flag(t->bar, LV_OBJ_FLAG_SCROLLABLE);

    t->bar_fill = lv_obj_create(t->bar);
    lv_obj_set_size(t->bar_fill, LV_PCT(0), LV_PCT(100));
    lv_obj_align(t->bar_fill, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(t->bar_fill, lv_color_hex(t->dim), 0);
    lv_obj_set_style_bg_opa(t->bar_fill, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(t->bar_fill, 0, 0);
    lv_obj_set_style_radius(t->bar_fill, 0, 0);
    lv_obj_set_style_pad_all(t->bar_fill, 0, 0);
    lv_obj_clear_flag(t->bar_fill, LV_OBJ_FLAG_SCROLLABLE);

    return t;
}

void scan_tile_set(scan_tile_t *t, int count, scan_severity_t sev) {
    if (!t) return;
    if (count == t->last_count && sev == t->last_sev) return;
    t->last_count = count;
    t->last_sev = sev;

    uint32_t color = sev_color(t, sev);
    lv_label_set_text_fmt(t->count_label, "%d", count);
    lv_obj_set_style_text_color(t->count_label, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(color), 0);

    int bar_val = count;
    if (bar_val > SCAN_TILE_BAR_MAX) bar_val = SCAN_TILE_BAR_MAX;
    if (bar_val < 0) bar_val = 0;
    lv_obj_set_width(t->bar_fill, LV_PCT(bar_val * 100 / SCAN_TILE_BAR_MAX));
    lv_obj_set_style_bg_color(t->bar_fill, lv_color_hex(color), 0);
}

lv_obj_t *scan_tile_get_obj(scan_tile_t *t) {
    return t ? t->card : NULL;
}

void scan_tile_destroy(scan_tile_t *t) {
    if (t) free(t);
}
