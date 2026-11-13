/*
 * app.h — the application: state, drawing (cairo) and input handling.
 *
 * The platform layer (plat_wayland.c) owns the window and the event loop and calls:
 *     app_init()      once, before the first frame
 *     app_draw()      when a->dirty is set (an 800x480 frame into a cairo_t)
 *     app_key()       a control-panel key (evdev code)
 *     app_touch()     a finger: TOUCH_DOWN / TOUCH_MOTION / TOUCH_UP
 *     app_tick()      periodically; sets a->exit_requested on key hold / idle timeout
 * and maintains a->visible / a->focused itself.
 * The application asks to quit via a->exit_requested (the Exit button etc.).
 */
#ifndef APP_H
#define APP_H

#include <stdint.h>
#include <cairo.h>

#define WIDTH   800
#define HEIGHT  480

#define MAX_TOUCH     10        /* simultaneous touches */
#define HIST_MAX      6         /* lines of key history */
#define HOLD_EXIT_MS  3000      /* holding any key -> exit (demo) */
#define IDLE_EXIT_MS  120000    /* no input -> exit (demo) */

enum { TOUCH_DOWN, TOUCH_MOTION, TOUCH_UP };

struct gfx {
    cairo_font_face_t *font, *font_bold;
    cairo_surface_t *bg;        /* demo: static background (grid, panels, targets) */
    cairo_surface_t *trail;     /* demo: touch trail layer (ARGB, accumulates) */
    cairo_t *trail_cr;
    int frame_ms;               /* render time of the last frame */
};

struct demo {
    struct { int x, y, down; } t[MAX_TOUCH];  /* active touches (id % MAX_TOUCH) */
    char hist[HIST_MAX][48];
    int hist_n;
    uint32_t last_key;
    int last_key_down, knob;
    unsigned key_events, touch_events;
    uint32_t held_key;          /* for "hold 3 s = exit" */
    long long held_since;
    int pressed_btn, pressed_id;  /* on-screen button under the finger */
    int last_x, last_y, last_id, have_last;
    long long last_input;
};

struct app {
    const char *name;           /* IVI surface name (for labels) */
    int demo_mode;              /* -d */
    int grab_keyboard;          /* -k (or -d) */
    int visible, focused;       /* window state — maintained by the platform */
    int dirty;                  /* needs a redraw */
    int exit_requested;         /* the application asks to quit */
    struct gfx g;
    struct demo d;
};

long long now_ms(void);

void app_init(struct app *a);
void app_draw(cairo_t *cr, struct app *a);
void app_key(struct app *a, uint32_t evdev_code, int down);
void app_touch(struct app *a, int kind, int id, int x, int y);
void app_touch_cancel(struct app *a);
void app_keyboard_leave(struct app *a);
/* Returns how many ms until it wants to be called again (event loop timeout). */
int  app_tick(struct app *a, long long now);

#endif
