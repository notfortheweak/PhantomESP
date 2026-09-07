/*
 * phantom_splash.c  --  PhantomESP animated boot splash for LVGL (v8.x)
 *
 * Effect: a spectral-green "sandstorm" blows in from the left. As its leading
 * edge sweeps right across the screen it uncovers the title "PhantomESP",
 * which glows on a black field. When the edge reaches the right side the storm
 * settles, the title snaps clean, and the frame holds.
 *
 * How it works (kept cheap for a plain ESP32 with software-rendered LVGL):
 *   - At start we render the finished, glowing title ONCE into an offscreen
 *     "master" buffer.
 *   - Each frame we compose a "live" buffer: copy the already-revealed columns
 *     straight from master (a memcpy per row), leave the rest black, then
 *     scatter sand particles around the moving reveal edge. Writing the canvas
 *     buffer directly + invalidating the canvas lets the existing LVGL task
 *     flush it -- we never re-run text layout per frame.
 *
 * Memory: two lv_color_t buffers of (hor_res x PHANTOM_SPLASH_BAND_H). On a
 * 240x320 panel with BAND_H=96 that's ~2 x 45 KB = ~90 KB, allocated at boot
 * (before Wi-Fi is hungry) and freed when the splash ends. If allocation
 * fails the splash is skipped and on_done fires immediately.
 *
 * ---- Generating the Rubik Glitch font (optional but recommended) ---------
 * The glitch look really comes from the glyphs. Convert the Google Font TTF:
 *
 *   lv_font_conv --font RubikGlitch-Regular.ttf --size 40 --bpp 4 \
 *       --range 0x20-0x7F --format lvgl --no-compress \
 *       -o rubik_glitch_40.c --force-fast-kern-format
 *
 *   (or the web tool at https://lvgl.io/tools/fontconverter -- name it
 *    "rubik_glitch_40", size 40, bpp 4, range 0x20-0x7F)
 *
 * Add rubik_glitch_40.c to your component's idf_component / CMakeLists SRCS,
 * then build with -DPHANTOM_SPLASH_FONT="(&rubik_glitch_40)" or edit the
 * default in phantom_splash.h. bpp 4 gives smooth glow edges.
 *
 * ---- LVGL v9 notes -------------------------------------------------------
 * If your tree is on LVGL v9: lv_canvas_set_buffer() signature changed and the
 * colour format enum is LV_COLOR_FORMAT_NATIVE (not LV_IMG_CF_TRUE_COLOR);
 * lv_canvas_draw_text() -> use a layer/lv_draw_label. The compose loop and
 * particle code below are version-independent (they touch the raw buffer).
 * The two lines that need attention are marked with "LVGL v9:".
 */

#include "phantom_splash.h"
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ state */

typedef struct {
    float x, y;      /* position in canvas coords            */
    float vx, vy;    /* velocity (px/frame)                  */
    int   life;      /* frames remaining                     */
    int   life0;     /* initial life (for fade ratio)        */
} particle_t;

typedef enum { PH_IDLE = 0, PH_REVEAL, PH_SETTLE, PH_HOLD } ph_phase_t;

static struct {
    lv_obj_t   *bg;            /* full-screen black backdrop      */
    lv_obj_t   *canvas;        /* the animated band               */
    lv_color_t *master;        /* finished glowing title          */
    lv_color_t *live;          /* per-frame composite (canvas buf)*/
    lv_timer_t *timer;

    int         w, h;          /* canvas dimensions               */
    ph_phase_t  phase;
    int         elapsed_ms;    /* time in current phase           */
    float       reveal_x;      /* leading edge, 0..w              */

    particle_t  parts[PHANTOM_SPLASH_PARTICLES];
    uint32_t    rng;

    void      (*on_done)(void);
    bool        running;
} S;

/* --------------------------------------------------------------- helpers */

/* small, self-contained PRNG so we don't lean on libc rand() timing */
static inline uint32_t ph_rand(void)
{
    uint32_t x = S.rng ? S.rng : 0xA5F00D1Eu;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    S.rng = x;
    return x;
}
static inline float ph_frand(void) { return (ph_rand() & 0xFFFF) / 65535.0f; }
static inline int   ph_irand(int lo, int hi) /* inclusive */
{ return lo + (int)(ph_rand() % (uint32_t)(hi - lo + 1)); }

static inline void put_px(int x, int y, lv_color_t c)
{
    if (x >= 0 && x < S.w && y >= 0 && y < S.h) S.live[y * S.w + x] = c;
}

/* ----------------------------------------------------- master rendering */

/* Render the glowing title once into S.master. */
static void render_master(void)
{
    const lv_font_t *font = PHANTOM_SPLASH_FONT;

    /* Bind the canvas to the master buffer temporarily and draw into it. */
    /* LVGL v9: replace LV_IMG_CF_TRUE_COLOR with LV_COLOR_FORMAT_NATIVE.  */
    lv_canvas_set_buffer(S.canvas, S.master, S.w, S.h, LV_IMG_CF_TRUE_COLOR);
    lv_canvas_fill_bg(S.canvas, lv_color_black(), LV_OPA_COVER);

    int line_h = lv_font_get_line_height(font);
    int y0 = (S.h - line_h) / 2;
    if (y0 < 0) y0 = 0;

    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.font  = font;
    dsc.align = LV_TEXT_ALIGN_CENTER;

    /* Halo: draw the text a few times, offset, in the dim glow colour. */
    dsc.color = lv_color_hex(PHANTOM_SPLASH_GREEN_GLOW);
    static const int8_t ox[] = { -2, 2, 0, 0, -1, 1, -1, 1 };
    static const int8_t oy[] = {  0, 0,-2, 2, -1,-1,  1, 1 };
    for (unsigned i = 0; i < sizeof(ox); i++)
        lv_canvas_draw_text(S.canvas, ox[i], y0 + oy[i], S.w, &dsc,
                            PHANTOM_SPLASH_TEXT);

    /* Bright spectral-green core on top. */
    dsc.color = lv_color_hex(PHANTOM_SPLASH_GREEN_CORE);
    lv_canvas_draw_text(S.canvas, 0, y0, S.w, &dsc, PHANTOM_SPLASH_TEXT);

    /* Hand the canvas its real (live) buffer for the animation. */
    lv_canvas_set_buffer(S.canvas, S.live, S.w, S.h, LV_IMG_CF_TRUE_COLOR);
}

/* --------------------------------------------------------- particle spawn */

static void spawn_particle(particle_t *p, float front_x)
{
    /* Sand is born in a band straddling the reveal edge and blows right. */
    p->x  = front_x + ph_frand() * 26.0f - 6.0f;
    p->y  = ph_frand() * (float)S.h;
    p->vx = 2.2f + ph_frand() * 3.4f;          /* rightward gust           */
    p->vy = (ph_frand() - 0.5f) * 1.6f;        /* slight vertical drift    */
    p->life0 = ph_irand(10, 26);
    p->life  = p->life0;
}

/* ------------------------------------------------------------ compositing */

static void compose_frame(void)
{
    int rx = (int)(S.reveal_x + 0.5f);
    if (rx < 0) rx = 0;
    if (rx > S.w) rx = S.w;

    /* Occasional horizontal "glitch" slice near the moving edge. */
    int glitch_y0 = -1, glitch_y1 = -1, glitch_off = 0;
    if (S.phase == PH_REVEAL && (ph_rand() & 7) == 0) {
        int gy = ph_irand(0, S.h - 1);
        int gh = ph_irand(2, 6);
        glitch_y0 = gy;
        glitch_y1 = gy + gh;
        glitch_off = ph_irand(-5, 5);
    }

    for (int y = 0; y < S.h; y++) {
        lv_color_t *dst = &S.live[y * S.w];
        lv_color_t *src = &S.master[y * S.w];

        /* whole row black first (0x0000 == black regardless of swap) */
        memset(dst, 0, (size_t)S.w * sizeof(lv_color_t));

        if (rx > 0) {
            if (y >= glitch_y0 && y < glitch_y1 && glitch_off != 0) {
                for (int x = 0; x < rx; x++) {
                    int sx = x - glitch_off;
                    if (sx >= 0 && sx < S.w) dst[x] = src[sx];
                }
            } else {
                memcpy(dst, src, (size_t)rx * sizeof(lv_color_t));
            }
        }
    }

    /* draw + advance particles */
    lv_color_t sand = lv_color_hex(PHANTOM_SPLASH_SAND_TINT);
    lv_color_t core = lv_color_hex(PHANTOM_SPLASH_GREEN_CORE);
    for (int i = 0; i < PHANTOM_SPLASH_PARTICLES; i++) {
        particle_t *p = &S.parts[i];
        if (p->life <= 0) continue;

        /* fade from sandy core -> dim green -> gone over its life */
        uint8_t mix = (uint8_t)(255 * p->life / (p->life0 ? p->life0 : 1));
        lv_color_t c = lv_color_mix(sand, core, mix);
        c = lv_color_mix(c, lv_color_black(), mix); /* darken as it dies */

        int px = (int)p->x, py = (int)p->y;
        put_px(px,     py,     c);
        put_px(px + 1, py,     c);
        put_px(px,     py + 1, c);           /* 2x2 grain for visibility */
        put_px(px + 1, py + 1, c);

        p->x += p->vx; p->y += p->vy;
        p->vy += 0.05f;                       /* faint settling */
        p->life--;
    }

    lv_obj_invalidate(S.canvas);
}

/* --------------------------------------------------------------- teardown */

static void finish_and_cleanup(void)
{
    if (!S.running) return;
    S.running = false;

    if (S.timer)  { lv_timer_del(S.timer); S.timer = NULL; }
    if (S.canvas) { lv_obj_del(S.canvas);  S.canvas = NULL; }
    if (S.bg)     { lv_obj_del(S.bg);      S.bg = NULL; }
    if (S.master) { free(S.master);        S.master = NULL; }
    if (S.live)   { free(S.live);          S.live = NULL; }

    void (*cb)(void) = S.on_done;
    S.on_done = NULL;
    S.phase = PH_IDLE;
    if (cb) cb();
}

/* ------------------------------------------------------------- timer step */

static void step_cb(lv_timer_t *t)
{
    (void)t;
    if (!S.running) return;

    S.elapsed_ms += PHANTOM_SPLASH_FRAME_MS;

    switch (S.phase) {
    case PH_REVEAL: {
        float frac = (float)S.elapsed_ms / (float)PHANTOM_SPLASH_REVEAL_MS;
        if (frac > 1.0f) frac = 1.0f;
        /* ease-out so the storm decelerates as it lands */
        float eased = 1.0f - (1.0f - frac) * (1.0f - frac);
        S.reveal_x = eased * (float)S.w;

        /* keep spawning sand at the front while sweeping */
        int budget = PHANTOM_SPLASH_PARTICLES / 12;
        for (int i = 0; i < PHANTOM_SPLASH_PARTICLES && budget > 0; i++) {
            if (S.parts[i].life <= 0) { spawn_particle(&S.parts[i], S.reveal_x); budget--; }
        }
        compose_frame();

        if (frac >= 1.0f) { S.phase = PH_SETTLE; S.elapsed_ms = 0; }
        break;
    }
    case PH_SETTLE:
        S.reveal_x = (float)S.w;      /* fully revealed; let sand die off */
        compose_frame();
        if (S.elapsed_ms >= PHANTOM_SPLASH_SETTLE_MS) {
            S.phase = PH_HOLD; S.elapsed_ms = 0;
            compose_frame();          /* one clean frame, no live particles */
        }
        break;

    case PH_HOLD:
        if (PHANTOM_SPLASH_HOLD_MS > 0 &&
            S.elapsed_ms >= PHANTOM_SPLASH_HOLD_MS) {
            finish_and_cleanup();
        }
        /* HOLD_MS <= 0 -> hold until phantom_splash_stop() */
        break;

    default:
        break;
    }
}

/* ------------------------------------------------------------------- API */

void phantom_splash_start(lv_obj_t *parent, void (*on_done)(void))
{
    if (S.running) return;
    memset(&S, 0, sizeof(S));
    S.on_done = on_done;
    S.rng     = 0xC0FFEEu ^ (uint32_t)lv_tick_get();

    if (!parent) parent = lv_scr_act();

    S.w = lv_obj_get_width(lv_scr_act());
    if (S.w <= 0) S.w = LV_HOR_RES;         /* fallback */
    S.h = PHANTOM_SPLASH_BAND_H;

    size_t bytes = (size_t)S.w * S.h * sizeof(lv_color_t);
    S.master = (lv_color_t *)malloc(bytes);
    S.live   = (lv_color_t *)malloc(bytes);
    if (!S.master || !S.live) {             /* low memory: skip gracefully */
        if (S.master) { free(S.master); S.master = NULL; }
        if (S.live)   { free(S.live);   S.live = NULL; }
        if (on_done) on_done();
        return;
    }

    /* full-screen black backdrop so nothing behind shows through */
    S.bg = lv_obj_create(parent);
    lv_obj_remove_style_all(S.bg);
    lv_obj_set_size(S.bg, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(S.bg, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(S.bg, LV_OPA_COVER, 0);
    lv_obj_clear_flag(S.bg, LV_OBJ_FLAG_SCROLLABLE);

    /* the animated band, centred vertically */
    S.canvas = lv_canvas_create(S.bg);

    S.running = true;
    S.phase   = PH_REVEAL;
    S.elapsed_ms = 0;
    S.reveal_x   = 0.0f;

    render_master();     /* draw glowing title into master, rebind live */

    /* Size + centre AFTER the buffer is bound, or the band lands off-centre. */
    lv_obj_set_size(S.canvas, S.w, S.h);
    lv_obj_align(S.canvas, LV_ALIGN_CENTER, 0, 0);

    compose_frame();     /* first frame = all black + a little edge sand */

    S.timer = lv_timer_create(step_cb, PHANTOM_SPLASH_FRAME_MS, NULL);
}

void phantom_splash_stop(void)
{
    finish_and_cleanup();
}

bool phantom_splash_active(void)
{
    return S.running;
}
