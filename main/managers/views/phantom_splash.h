/*
 * phantom_splash.h  --  PhantomESP animated boot splash for LVGL displays
 *
 * A ghostly "sandstorm" sweeps left -> right and reveals the title
 * "PhantomESP" in glowing spectral green on black, then holds.
 *
 * Display-driver agnostic: works on any GhostESP/PhantomESP target whose
 * UI is backed by LVGL (CYD2432S028R and the other CYD variants, the
 * Waveshare/Sunton/Crowtech LCD boards, T-Deck, etc.). It draws to the
 * active LVGL screen through a canvas, so it does not care which physical
 * panel/driver is underneath.
 *
 * Target LVGL: v8.x  (see notes in the .c for v9 API differences).
 *
 * ---- Quick use -----------------------------------------------------------
 *   #include "phantom_splash.h"
 *   ...
 *   // after LVGL + the display driver are initialised, and BEFORE you build
 *   // your normal menu/home screen:
 *   phantom_splash_start(lv_scr_act(), on_splash_done);
 *
 *   static void on_splash_done(void) {
 *       // splash has finished + freed itself; build the real UI now
 *       ui_show_main_menu();
 *   }
 *
 * If you would rather block (simple boot flows), you can spin your existing
 * LVGL tick/handler loop until on_done fires -- the module itself never
 * blocks and never calls lv_timer_handler() for you.
 * -------------------------------------------------------------------------
 */

#ifndef PHANTOM_SPLASH_H
#define PHANTOM_SPLASH_H

#include "lvgl.h"          /* if your tree uses a subdir, change to "lvgl/lvgl.h" */

#ifdef __cplusplus
extern "C" {
#endif

/* ============================ TUNABLES ================================= */

/* Title text. */
#ifndef PHANTOM_SPLASH_TEXT
#define PHANTOM_SPLASH_TEXT "PhantomESP"
#endif

/*
 * Font.
 * Default is a built-in Montserrat so this compiles and runs out of the box.
 * To use Rubik Glitch (the real look): generate an LVGL font from the TTF
 * (see the header of phantom_splash.c for the exact lv_font_conv command),
 * add the produced .c to your component SRCS, then:
 *
 *     extern const lv_font_t rubik_glitch_40;
 *     #define PHANTOM_SPLASH_FONT (&rubik_glitch_40)
 *
 * before including this header (or just edit the line below).
 */
#ifndef PHANTOM_SPLASH_FONT
extern const lv_font_t rubik_glitch_28;
#define PHANTOM_SPLASH_FONT (&rubik_glitch_28)
#endif

/* Spectral green palette (hex RGB). Core is the bright glow colour. */
#ifndef PHANTOM_SPLASH_GREEN_CORE
#define PHANTOM_SPLASH_GREEN_CORE  0x39FF14   /* glowing spectral green      */
#endif
#ifndef PHANTOM_SPLASH_GREEN_GLOW
#define PHANTOM_SPLASH_GREEN_GLOW  0x0B5D0B   /* dim halo around the glyphs  */
#endif
#ifndef PHANTOM_SPLASH_SAND_TINT
#define PHANTOM_SPLASH_SAND_TINT   0xBFFFB0   /* near-white green sand cores */
#endif

/* Canvas band height (px). The band is full display width, centred
 * vertically. Two RGB565 buffers of (width x this) are allocated during the
 * splash and freed afterwards. Lower this if a board is tight on RAM. */
#ifndef PHANTOM_SPLASH_BAND_H
#define PHANTOM_SPLASH_BAND_H  96
#endif

/* Animation timing (ms). */
#ifndef PHANTOM_SPLASH_REVEAL_MS
#define PHANTOM_SPLASH_REVEAL_MS  1700    /* left->right sweep duration      */
#endif
#ifndef PHANTOM_SPLASH_SETTLE_MS
#define PHANTOM_SPLASH_SETTLE_MS  550     /* let trailing sand die off       */
#endif
#ifndef PHANTOM_SPLASH_HOLD_MS
#define PHANTOM_SPLASH_HOLD_MS    1600    /* hold finished title; <=0 = hold
                                             forever until _stop() is called */
#endif
#ifndef PHANTOM_SPLASH_FRAME_MS
#define PHANTOM_SPLASH_FRAME_MS   33      /* ~30 fps                         */
#endif

/* Number of live sand particles. More = denser storm, a little more CPU. */
#ifndef PHANTOM_SPLASH_PARTICLES
#define PHANTOM_SPLASH_PARTICLES  190
#endif

/* ============================ API ===================================== */

/*
 * Start the splash on `parent` (usually lv_scr_act()).
 * `on_done` is called once, from the LVGL context, after the hold phase;
 * by then the splash has deleted its objects and freed its buffers.
 * `on_done` may be NULL.
 *
 * If the required buffers cannot be allocated, the splash is skipped and
 * `on_done` is invoked (near-)immediately so boot never stalls.
 */
void phantom_splash_start(lv_obj_t *parent, void (*on_done)(void));

/* End the splash early (e.g. on a key/touch press, or when HOLD_MS <= 0).
 * Safe to call even if no splash is running. Triggers on_done + cleanup. */
void phantom_splash_stop(void);

/* True while the splash is on screen. */
bool phantom_splash_active(void);

#ifdef __cplusplus
}
#endif

#endif /* PHANTOM_SPLASH_H */
