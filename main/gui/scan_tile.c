// scan_tile.c — see header.
#include "gui/scan_tile.h"
#include "gui/theme_palette_api.h"
#include "managers/settings_manager.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define SCAN_TILE_BAR_MAX 20     // total magnitude at which the level bar is full
#define SCAN_TILE_TOTAL_COLOR 0xB388FF  // purple "N seen" (matches SCAN_COLOR_TOTAL)

struct scan_tile_t {
    lv_obj_t *card;
    lv_obj_t *count_label;
    lv_obj_t *total_label;
    lv_obj_t *bar;        // level-bar track (plain lv_obj; lv_bar isn't built on
                          // every board's LVGL config, so we roll our own)
    lv_obj_t *bar_fill;   // indicator child, width set as a percentage of track
    char last_primary[24];
    int last_total;
    scan_severity_t last_sev;
    // palette (captured at create so update() stays theme-consistent)
    uint32_t surface;
    uint32_t text;
    uint32_t dim;
    uint32_t sev_present;
    uint32_t sev_threat;
    uint32_t accent;
    // scanning-pulse state, so restyling only happens when it actually changes
    bool last_scanning;
    bool last_phase;
};

static uint32_t sev_color(const scan_tile_t *t, scan_severity_t sev) {
    switch (sev) {
    case SCAN_SEV_THREAT:  return t->sev_threat;
    case SCAN_SEV_PRESENT: return t->sev_present;
    default:               return t->dim;
    }
}

scan_tile_t *scan_tile_create(lv_obj_t *parent, const char *label, const char *icon) {
    if (!parent) return NULL;
    scan_tile_t *t = calloc(1, sizeof(scan_tile_t));
    if (!t) return NULL;

    uint8_t theme = settings_get_menu_theme(&G_Settings);
    t->surface     = theme_palette_get_surface_alt(theme);
    t->text        = theme_palette_get_text(theme);
    t->dim         = theme_palette_get_text_muted(theme);
    t->accent      = theme_palette_get_accent(theme);
    t->sev_present = 0xFFAA00;  // amber
    t->sev_threat  = 0xFF4444;  // red
    t->last_primary[0] = '\0';
    t->last_total  = -1;
    t->last_sev    = (scan_severity_t)-1;
    t->last_scanning = false;
    t->last_phase    = false;

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

    // Icon + label share one line. The icon is a font glyph, so this is a single
    // extra label rather than an image -- important on the no-PSRAM boards where
    // LVGL's builtin pool is only ~20KB.
    lv_obj_t *title = lv_label_create(t->card);
    if (icon && icon[0]) lv_label_set_text_fmt(title, "%s %s", icon, label ? label : "");
    else                 lv_label_set_text(title, label ? label : "");
    lv_obj_set_style_text_color(title, lv_color_hex(t->dim), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, LV_PCT(100));

    t->count_label = lv_label_create(t->card);
    lv_label_set_text(t->count_label, "0");
    lv_obj_set_style_text_color(t->count_label, lv_color_hex(t->dim), 0);
    lv_obj_set_style_text_font(t->count_label, &lv_font_montserrat_24, 0);

    t->total_label = lv_label_create(t->card);          // "N seen" (session total)
    lv_label_set_text(t->total_label, "0 seen");
    lv_obj_set_style_text_color(t->total_label, lv_color_hex(SCAN_TILE_TOTAL_COLOR), 0);

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

void scan_tile_set(scan_tile_t *t, const char *primary, int total, scan_severity_t sev) {
    if (!t) return;
    if (!primary) primary = "0";
    if (total == t->last_total && sev == t->last_sev &&
        strncmp(primary, t->last_primary, sizeof(t->last_primary)) == 0) {
        return;
    }
    snprintf(t->last_primary, sizeof(t->last_primary), "%s", primary);
    t->last_total = total;
    t->last_sev = sev;

    uint32_t color = sev_color(t, sev);
    lv_label_set_text(t->count_label, primary);
    lv_obj_set_style_text_color(t->count_label, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(t->card, lv_color_hex(color), 0);
    lv_label_set_text_fmt(t->total_label, "%d seen", total);

    int bar_val = total;
    if (bar_val > SCAN_TILE_BAR_MAX) bar_val = SCAN_TILE_BAR_MAX;
    if (bar_val < 0) bar_val = 0;
    lv_obj_set_width(t->bar_fill, LV_PCT(bar_val * 100 / SCAN_TILE_BAR_MAX));
    lv_obj_set_style_bg_color(t->bar_fill, lv_color_hex(color), 0);
}

lv_obj_t *scan_tile_get_obj(scan_tile_t *t) {
    return t ? t->card : NULL;
}

void scan_tile_set_scanning(scan_tile_t *t, bool scanning, bool phase) {
    if (!t || !t->card || !lv_obj_is_valid(t->card)) return;
    if (t->last_scanning == scanning && (!scanning || t->last_phase == phase)) return;
    t->last_scanning = scanning;
    t->last_phase = phase;

    if (scanning) {
        // Pulse: alternate a bright full accent border with the thin resting one.
        // The caller flips `phase` on its periodic tick to animate it.
        lv_obj_set_style_border_color(t->card, lv_color_hex(phase ? t->accent : t->dim), 0);
        lv_obj_set_style_border_width(t->card, phase ? 4 : 2, 0);
        lv_obj_set_style_border_side(t->card, phase ? LV_BORDER_SIDE_FULL : LV_BORDER_SIDE_LEFT, 0);
    } else {
        // Restore the resting look. scan_tile_set() owns the severity color, so
        // invalidate its cache to force a repaint on the next update.
        lv_obj_set_style_border_width(t->card, 2, 0);
        lv_obj_set_style_border_side(t->card, LV_BORDER_SIDE_LEFT, 0);
        t->last_sev = (scan_severity_t)-1;
    }
}

void scan_tile_destroy(scan_tile_t *t) {
    if (t) free(t);
}
