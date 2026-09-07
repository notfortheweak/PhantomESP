#include "managers/views/splash_screen.h"
#include "managers/views/main_menu_screen.h"
#include "managers/views/scan_dashboard_screen.h"
#include "managers/views/setup_wizard_screen.h"
#include "managers/views/music_visualizer.h"
#include "managers/views/lockscreen.h"
#include "managers/settings_manager.h"
#include "core/ghostesp_version.h"
#include "gui/screen_layout.h"
#include "gui/lvgl_safe.h"
#include "phantom_splash.h"
#include <stdio.h>
#include <string.h>


#define SPLASH_MIN_HOLD_MS_REDUCED  50
#define SPLASH_MIN_HOLD_MS_NORMAL   2200
#define SPLASH_TIMEOUT_MS           8000
#define SPLASH_PROGRESS_BAR_HEIGHT  8
#define SPLASH_LABEL_MAX_LEN        32
#define SPLASH_PROGRESS_BAR_INDETERMINATE_PCT (-1.0f)
#define SPLASH_INDETERMINATE_PERIOD_MS 1200
#define SPLASH_PROGRESS_BAR_WIDTH_PCT  60
#define SPLASH_PROGRESS_BAR_MIN_WIDTH  80

lv_obj_t *splash_screen;
lv_obj_t *img;

static lv_obj_t *s_status_label = NULL;
static lv_obj_t *s_progress_bar = NULL;
static lv_obj_t *s_progress_fill = NULL;
static lv_coord_t s_progress_bar_width = 0;
static lv_timer_t *s_fade_timer = NULL;
static lv_anim_t s_progress_indet_anim;
static bool s_progress_indet_running = false;
static uint32_t s_splash_start_ms = 0;
static bool s_splash_done = false;
static bool s_completion_required = false;

typedef struct {
    float pct;
    char label[SPLASH_LABEL_MAX_LEN];
} splash_progress_msg_t;

static void fade_out_cb(void *var);
static void splash_hold_expired_cb(lv_timer_t *t);
static void splash_progress_apply(void *arg);
static void splash_completion_apply(void *arg);
static void splash_progress_indet_anim_cb(void *var, int32_t v);
static void start_indeterminate_anim(void);
static void stop_indeterminate_anim(void);
static uint32_t min_hold_ms(void);
static uint32_t elapsed_ms(void);
static void schedule_fade_check(void);

/* --- Lightweight glitch-reveal wordmark (no big buffers; low-memory boards) --- */
static lv_obj_t   *s_pe_label  = NULL;
static lv_timer_t *s_pe_timer  = NULL;
static uint8_t     s_pe_reveal = 0;
static uint8_t     s_pe_tick   = 0;
static uint32_t    s_pe_rng    = 0x50E7u;
static const char  PE_TEXT[]   = "PhantomESP";
static const char  PE_GLITCH[] = "#%&$@!*/<>?+=";

static uint32_t pe_rand(void){ uint32_t x=s_pe_rng?s_pe_rng:0xA5F0u; x^=x<<13; x^=x>>17; x^=x<<5; s_pe_rng=x; return x; }

static void pe_reveal_cb(lv_timer_t *t){
    (void)t;
    if(!s_pe_label){ if(s_pe_timer){ lv_timer_del(s_pe_timer); s_pe_timer=NULL; } return; }
    const int n=(int)(sizeof(PE_TEXT)-1);
    if(s_pe_reveal >= (uint8_t)n){
        lv_label_set_text(s_pe_label, PE_TEXT);
        if(s_pe_timer){ lv_timer_del(s_pe_timer); s_pe_timer=NULL; }
        return;
    }
    char buf[24]; int k=0;
    for(int i=0;i<(int)s_pe_reveal && k<20;i++) buf[k++]=PE_TEXT[i];
    buf[k++]=PE_GLITCH[pe_rand()%(sizeof(PE_GLITCH)-1)];   /* flickering cursor */
    buf[k]='\0';
    lv_label_set_text(s_pe_label, buf);
    if((++s_pe_tick)>=2){ s_pe_tick=0; s_pe_reveal++; }
}

static void splash_require_completion_apply(void *arg) {
    (void)arg;
    schedule_fade_check();
}

void splash_require_completion(void) {
    s_completion_required = true;
    display_manager_run_on_lvgl(splash_require_completion_apply, NULL);
}

static uint32_t min_hold_ms(void) {
    return settings_get_reduced_motion(&G_Settings) ? SPLASH_MIN_HOLD_MS_REDUCED : SPLASH_MIN_HOLD_MS_NORMAL;
}

static uint32_t elapsed_ms(void) {
    return lv_tick_elaps(s_splash_start_ms);
}

static void phantom_splash_boot_done(void) {
    /* Animated splash finished: mark done so the existing fade/routing runs. */
    s_splash_done = true;
    schedule_fade_check();
}

void splash_create(void) {

  s_completion_required = false;

  display_manager_fill_screen(lv_color_black());

  splash_screen = gui_screen_create_root_no_bg(NULL, NULL, lv_color_black(), LV_OPA_COVER);
  splash_view.root = splash_screen;

  /* Progress-bar UI retired; the animated PhantomESP splash is the boot visual
   * and drives the transition to the destination via phantom_splash_boot_done(). */
  s_status_label = NULL;
  s_progress_bar = NULL;
  s_progress_fill = NULL;

  s_splash_start_ms = lv_tick_get();
  s_splash_done = false;
  s_progress_indet_running = false;

  /* Animated sandstorm splash. It needs ~2x (width x band) RGB565 buffers and
   * self-skips on low-memory (no-PSRAM) boards. When it skips, show a
   * lightweight Rubik-Glitch "PhantomESP" wordmark instead, so every board still
   * gets a branded boot screen. */
  phantom_splash_start(splash_screen, phantom_splash_boot_done);
  if (!phantom_splash_active()) {
    s_pe_reveal = 0; s_pe_tick = 0; s_pe_rng ^= lv_tick_get();
    s_pe_label = lv_label_create(splash_screen);
    lv_label_set_text(s_pe_label, "");
    lv_obj_set_style_text_font(s_pe_label, &rubik_glitch_28, 0);
    lv_obj_set_style_text_color(s_pe_label, lv_color_hex(0x39FF14), 0);
    lv_obj_center(s_pe_label);
    lv_obj_fade_in(s_pe_label, 400, 0);
    s_pe_timer = lv_timer_create(pe_reveal_cb, 60, NULL);
  }

  schedule_fade_check();
}

static void splash_progress_indet_anim_cb(void *var, int32_t v) {
    (void)var;
    if (!s_progress_fill) return;
    int32_t bar_w = (int32_t)s_progress_bar_width;
    int32_t pill_w = bar_w / 3;
    if (pill_w < 8) pill_w = 8;
    int32_t max_x = bar_w - pill_w;
    if (max_x < 0) max_x = 0;
    int32_t x = (v * max_x) / 100;
    lv_obj_set_size(s_progress_fill, (lv_coord_t)pill_w, SPLASH_PROGRESS_BAR_HEIGHT);
    lv_obj_set_x(s_progress_fill, (lv_coord_t)x);
}

static void start_indeterminate_anim(void) {
    if (!s_progress_fill || s_progress_indet_running) return;
    if (settings_get_reduced_motion(&G_Settings)) return;
    s_progress_indet_running = true;
    lv_anim_init(&s_progress_indet_anim);
    lv_anim_set_var(&s_progress_indet_anim, s_progress_fill);
    lv_anim_set_values(&s_progress_indet_anim, 0, 100);
    lv_anim_set_time(&s_progress_indet_anim, SPLASH_INDETERMINATE_PERIOD_MS);
    lv_anim_set_playback_time(&s_progress_indet_anim, 0);
    lv_anim_set_repeat_count(&s_progress_indet_anim, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_exec_cb(&s_progress_indet_anim, splash_progress_indet_anim_cb);
    lv_anim_start(&s_progress_indet_anim);
}

static void stop_indeterminate_anim(void) {
    if (!s_progress_indet_running) return;
    lv_anim_del(s_progress_fill, splash_progress_indet_anim_cb);
    s_progress_indet_running = false;
}

static void splash_hold_expired_cb(lv_timer_t *t) {
    (void)t;
    s_fade_timer = NULL;
    fade_out_cb(NULL);
}

static void fade_out_cb(void *var) {
  (void)var;
  stop_indeterminate_anim();
  if (s_fade_timer) {
      lv_timer_del(s_fade_timer);
      s_fade_timer = NULL;
  }
  if (!settings_get_setup_complete(&G_Settings)) {
    display_manager_switch_view(&setup_wizard_view);
  } else if (settings_get_lockscreen_enabled(&G_Settings)) {
    lockscreen_reset_input();
    display_manager_switch_view(&lockscreen_view);
  } else {
    // Auto-boot into the live scan dashboard (appliance mode); the menu is one
    // input away via the dashboard's exit handler.
    display_manager_switch_view(&scan_dashboard_view);
  }
}

static void schedule_fade_check(void) {
    if (s_fade_timer) {
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
    }
    uint32_t elapsed = elapsed_ms();
    uint32_t min_hold = min_hold_ms();
    uint32_t delay;
    if (s_splash_done) {
        if (elapsed >= min_hold) {
            delay = 0;
        } else {
            delay = min_hold - elapsed;
        }
    } else if (s_completion_required) {
        return;
    } else {
        if (elapsed >= SPLASH_TIMEOUT_MS) {
            delay = 0;
        } else {
            delay = SPLASH_TIMEOUT_MS - elapsed;
        }
    }
    s_fade_timer = lv_timer_create(splash_hold_expired_cb, delay, NULL);
    lv_timer_set_repeat_count(s_fade_timer, 1);
}

void splash_set_progress(float pct, const char *label) {
    splash_progress_msg_t *msg = malloc(sizeof(*msg));
    if (!msg) return;
    if (pct < 0.0f) {
        msg->pct = SPLASH_PROGRESS_BAR_INDETERMINATE_PCT;
    } else {
        if (pct > 100.0f) pct = 100.0f;
        msg->pct = pct;
    }
    if (label) {
        strncpy(msg->label, label, SPLASH_LABEL_MAX_LEN - 1);
        msg->label[SPLASH_LABEL_MAX_LEN - 1] = '\0';
    } else {
        msg->label[0] = '\0';
    }
    display_manager_run_on_lvgl(splash_progress_apply, msg);
}

static void splash_progress_apply(void *arg) {
    splash_progress_msg_t *msg = (splash_progress_msg_t *)arg;
    if (!msg) return;
    if (s_status_label && msg->label[0]) {
        lv_label_set_text(s_status_label, msg->label);
    }
    if (s_progress_fill && s_progress_bar) {
        if (msg->pct < 0.0f) {
            if (!s_progress_indet_running) start_indeterminate_anim();
        } else {
            if (s_progress_indet_running) stop_indeterminate_anim();
            int32_t w = (int32_t)(((float)s_progress_bar_width * msg->pct) / 100.0f + 0.5f);
            if (w < 0) w = 0;
            if (w > s_progress_bar_width) w = s_progress_bar_width;
            lv_obj_set_size(s_progress_fill, (lv_coord_t)w, SPLASH_PROGRESS_BAR_HEIGHT);
            lv_obj_set_x(s_progress_fill, 0);
        }
    }
    free(msg);
}

void splash_signal_completion(void) {
    display_manager_run_on_lvgl(splash_completion_apply, NULL);
}

static void splash_completion_apply(void *arg) {
    (void)arg;
    s_splash_done = true;
    stop_indeterminate_anim();
    if (s_progress_fill && s_progress_bar) {
        lv_obj_set_size(s_progress_fill, s_progress_bar_width, SPLASH_PROGRESS_BAR_HEIGHT);
        lv_obj_set_x(s_progress_fill, 0);
    }
    if (s_status_label) {
        lv_label_set_text(s_status_label, "Ready");
    }
    schedule_fade_check();
}

void splash_destroy(void) {
    if (phantom_splash_active()) phantom_splash_stop();
    if (s_pe_timer) { lv_timer_del(s_pe_timer); s_pe_timer = NULL; }
    s_pe_label = NULL;
    stop_indeterminate_anim();
    if (s_fade_timer) {
        lv_timer_del(s_fade_timer);
        s_fade_timer = NULL;
    }
    s_status_label = NULL;
    s_progress_bar = NULL;
    s_progress_fill = NULL;
    lvgl_obj_del_safe(&splash_screen);
}

View splash_view = {.root = NULL,
                    .create = splash_create,
                    .destroy = splash_destroy,
                    .input_callback = NULL,
                    .name = "Splash Screen"};
