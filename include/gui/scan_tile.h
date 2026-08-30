// scan_tile.h
// A small dashboard tile: category label + big live count + severity color +
// magnitude level-bar. Reusable LVGL widget (peer of rssi_meter / live_chart).
// Create/update/destroy from the LVGL task only.
#pragma once

#include "lvgl.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SCAN_SEV_IDLE = 0,   // nothing seen (muted)
    SCAN_SEV_PRESENT,    // benign/ambient detections (amber)
    SCAN_SEV_THREAT,     // hostile/surveillance detections (red)
} scan_severity_t;

typedef struct scan_tile_t scan_tile_t;

// Create a tile inside `parent`. Width defaults to 100% of its flex slot; the
// caller can override via lv_obj_set_width(scan_tile_get_obj(t), ...).
// `icon` is an LVGL symbol string (e.g. LV_SYMBOL_WIFI) shown beside the label,
// or NULL for none. Symbols are glyphs in the already-compiled font, so they cost
// no flash and almost nothing from LVGL's small builtin pool.
scan_tile_t *scan_tile_create(lv_obj_t *parent, const char *label, const char *icon);

// Update the tile: `primary` is the big headline text (e.g. "3" or "A:2 S:5"),
// `total` is the running session total shown as "N seen", `sev` colors the
// headline/border. Cheap; skips work when nothing changed.
void scan_tile_set(scan_tile_t *tile, const char *primary, int total, scan_severity_t sev);

// The tile's root object, for sizing/placement in the parent's layout.
lv_obj_t *scan_tile_get_obj(scan_tile_t *tile);

// Frees the wrapper struct. LVGL objects are freed with their parent.
// Mark the tile as the category being scanned right now. `phase` alternates the
// highlight so the caller's periodic tick produces a pulse. Only restyles the
// existing card -- no new objects, no LVGL animation engine.
void scan_tile_set_scanning(scan_tile_t *tile, bool scanning, bool phase);

void scan_tile_destroy(scan_tile_t *tile);

#ifdef __cplusplus
}
#endif
