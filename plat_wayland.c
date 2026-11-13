/*
 * plat_wayland.c — the platform layer: a Wayland + IVI window on the Mazda CMU.
 *
 *   1. connects to the compositor (wayland-viv-compositor),
 *   2. creates an 800x480 wl_surface with two wl_shm buffers (app.c draws into them with cairo),
 *   3. registers the surface under the name $WAYLAND_IVI_SURFACE_ID (wl_ivi),
 *   4. shows/hides it (alpha + z-order via wl_ivi_shell),
 *   5. optionally takes the keyboard focus (knob/buttons) and gives it back to the HMI,
 *   6. forwards keys and touches to app.c.
 *
 * Process control:
 *   SIGUSR1 — show (and take the focus with -k/-d)
 *   SIGUSR2 — hide (and give the focus back to the HMI)
 *   SIGTERM / SIGINT / -t timeout — hide, give the focus back, exit.
 *
 * No wayland headers needed: the firmware has Wayland ~1.4, all requests go through
 * wl_proxy_marshal*() with opcodes taken from the interface tables of the firmware's
 * libwayland-*.so.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <poll.h>
#include <sys/mman.h>
#include "app.h"

/* ------------------------------------------------------------------ */
/* The firmware's libwayland-client / libwayland-ivi-*-client         */
/* ------------------------------------------------------------------ */
struct wl_display;
struct wl_proxy;

struct wl_interface {
    const char *name;
    int version;
    int method_count;
    const void *methods;
    int event_count;
    const void *events;
};

struct wl_display *wl_display_connect(const char *name);
void wl_display_disconnect(struct wl_display *display);
int  wl_display_get_fd(struct wl_display *display);
int  wl_display_roundtrip(struct wl_display *display);
int  wl_display_flush(struct wl_display *display);
int  wl_display_dispatch_pending(struct wl_display *display);
int  wl_display_prepare_read(struct wl_display *display);
int  wl_display_read_events(struct wl_display *display);
void wl_display_cancel_read(struct wl_display *display);
void wl_proxy_destroy(struct wl_proxy *proxy);
int  wl_proxy_add_listener(struct wl_proxy *proxy, void (**impl)(void), void *data);
struct wl_proxy *wl_proxy_marshal_constructor(struct wl_proxy *proxy, uint32_t opcode,
                                              const struct wl_interface *iface, ...);
void wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...);

extern struct wl_interface wl_registry_interface[];
extern struct wl_interface wl_compositor_interface[];
extern struct wl_interface wl_shm_interface[];
extern struct wl_interface wl_shm_pool_interface[];
extern struct wl_interface wl_buffer_interface[];
extern struct wl_interface wl_surface_interface[];
extern struct wl_interface wl_seat_interface[];
extern struct wl_interface wl_keyboard_interface[];
extern struct wl_interface wl_pointer_interface[];
extern struct wl_interface wl_touch_interface[];
extern struct wl_interface wl_ivi_interface[];
extern struct wl_interface wl_ivi_shell_interface[];
extern struct wl_interface wl_ivi_animation_group_interface[];

/* Request opcodes (from the firmware's interface tables) */
#define WL_DISPLAY_GET_REGISTRY        1   /* "n"      */
#define WL_REGISTRY_BIND               0   /* "usun"   */
#define WL_COMPOSITOR_CREATE_SURFACE   0   /* "n"      */
#define WL_SHM_CREATE_POOL             0   /* "nhi"    */
#define WL_SHM_POOL_CREATE_BUFFER      0   /* "niiiiu" */
#define WL_SURFACE_ATTACH              1   /* "?oii"   */
#define WL_SURFACE_DAMAGE              2   /* "iiii"   */
#define WL_SURFACE_COMMIT              6   /* ""       */
#define WL_SEAT_GET_POINTER            0   /* "n"      */
#define WL_SEAT_GET_KEYBOARD           1   /* "n"      */
#define WL_SEAT_GET_TOUCH              2   /* "n"      */
#define WL_IVI_REGISTER_SURFACE        0   /* "os"     */
#define WL_IVI_SHELL_CREATE_ANIM_GROUP 0   /* "n"      */
#define WL_IVI_SHELL_SET_KBD_FOCUS     1   /* "o?s"  (seat, surface name) */
#define WL_IVI_ANIM_START              0   /* ""       */
#define WL_IVI_ANIM_DESTROY            3   /* ""       */
#define WL_IVI_ANIM_ANIMATE_SCALAR     5   /* "suuufuu" name,prop,t0,t1,value,0,0 */

/* Properties for animate_scalar (taken from /usr/bin/ivi-shell) */
#define IVI_PROP_ZORDER  1                  /* value = zorder           */
#define IVI_PROP_ALPHA   2                  /* value = alpha/255        */

#define WL_SHM_FORMAT_XRGB8888   1
#define WL_SEAT_CAP_POINTER      1
#define WL_SEAT_CAP_KEYBOARD     2
#define WL_SEAT_CAP_TOUCH        4

/* The HMI surface that owns the keyboard focus in normal operation
 * (jci/gui/framework/js/GuiFramework.js: _OPERA_SURFACE). */
#define HMI_SURFACE  "JCI_OPERA_PRIMARY"
#define STRIDE      (WIDTH * 4)
#define FRAME_BYTES ((size_t)STRIDE * HEIGHT)
#define POINTER_ID  99          /* "finger" id for input via wl_pointer */

/* ------------------------------------------------------------------ */
/* Platform state                                                     */
/* ------------------------------------------------------------------ */
struct buf {
    struct wl_proxy *wl;
    uint32_t *px;
    cairo_surface_t *cs;        /* cairo surface over px */
    int busy;                   /* held by the compositor until release */
};

struct wl {
    struct wl_display *dpy;
    struct wl_proxy *compositor, *shm, *seat, *ivi, *ivi_shell;
    struct wl_proxy *keyboard, *pointer, *touch, *surface;
    struct buf bufs[2];
    int zorder, alpha;
    long long last_draw;
    int ptr_x, ptr_y, ptr_down;
    struct app app;
};

enum { CMD_NONE, CMD_SHOW, CMD_HIDE };
static volatile sig_atomic_t g_running = 1;
static volatile sig_atomic_t g_cmd = CMD_NONE;

static void on_signal(int sig)
{
    if (sig == SIGUSR1)
        g_cmd = CMD_SHOW;
    else if (sig == SIGUSR2)
        g_cmd = CMD_HIDE;
    else
        g_running = 0;
}

/* ------------------------------------------------------------------ */
/* Buffers and redraw                                                 */
/* ------------------------------------------------------------------ */
static void buffer_release(void *data, struct wl_proxy *wl_buffer)
{
    (void)wl_buffer;
    ((struct buf *)data)->busy = 0;
}

static const void *buffer_listener[] = { (void *)buffer_release };

/* Draw a frame into a free buffer and hand it to the compositor.
 * If both buffers are still held by the compositor, the frame stays dirty until release. */
static void redraw(struct wl *w)
{
    struct buf *b = !w->bufs[0].busy ? &w->bufs[0] : !w->bufs[1].busy ? &w->bufs[1] : NULL;
    cairo_t *cr;

    if (!b)
        return;
    cr = cairo_create(b->cs);
    app_draw(cr, &w->app);
    cairo_destroy(cr);
    cairo_surface_flush(b->cs);

    wl_proxy_marshal(w->surface, WL_SURFACE_ATTACH, b->wl, 0, 0);
    wl_proxy_marshal(w->surface, WL_SURFACE_DAMAGE, 0, 0, WIDTH, HEIGHT);
    wl_proxy_marshal(w->surface, WL_SURFACE_COMMIT);
    b->busy = 1;
    w->last_draw = now_ms();
}

/* ------------------------------------------------------------------ */
/* Visibility and focus                                               */
/* ------------------------------------------------------------------ */

/* Instantly set the surface z-order (if >= 0) and alpha — same as
 * `ivi-shell -z NAME:Z -a NAME:A`. */
static void ivi_set(struct wl *w, int zorder, int alpha)
{
    struct wl_proxy *g = wl_proxy_marshal_constructor(w->ivi_shell,
            WL_IVI_SHELL_CREATE_ANIM_GROUP, wl_ivi_animation_group_interface, NULL);
    if (!g)
        return;
    if (zorder >= 0)
        wl_proxy_marshal(g, WL_IVI_ANIM_ANIMATE_SCALAR, w->app.name,
                         (uint32_t)IVI_PROP_ZORDER, 0u, 0u, (int32_t)(zorder * 256), 0u, 0u);
    wl_proxy_marshal(g, WL_IVI_ANIM_ANIMATE_SCALAR, w->app.name,
                     (uint32_t)IVI_PROP_ALPHA, 0u, 0u, (int32_t)(alpha * 256 / 255), 0u, 0u);
    wl_proxy_marshal(g, WL_IVI_ANIM_START);
    wl_proxy_marshal(g, WL_IVI_ANIM_DESTROY);
    wl_proxy_destroy(g);
}

/* Give the keyboard focus to the surface with IVI name target —
 * same as `ivi-shell -f target`. */
static void ivi_focus(struct wl *w, const char *target)
{
    if (w->seat)
        wl_proxy_marshal(w->ivi_shell, WL_IVI_SHELL_SET_KBD_FOCUS, w->seat, target);
}

static void win_show(struct wl *w)
{
    struct app *a = &w->app;

    ivi_set(w, w->zorder, w->alpha);
    a->visible = 1;
    if (a->grab_keyboard && !a->focused) {
        ivi_focus(w, a->name);
        a->focused = 1;
    }
    a->dirty = 1;
    fprintf(stderr, "[cs] shown (zorder=%d alpha=%d%s)\n", w->zorder, w->alpha,
            a->focused ? ", keyboard grabbed" : "");
}

static void win_hide(struct wl *w)
{
    struct app *a = &w->app;

    if (a->focused) {
        ivi_focus(w, HMI_SURFACE);       /* the HMI won't take it back by itself */
        a->focused = 0;
        fprintf(stderr, "[cs] keyboard returned to " HMI_SURFACE "\n");
    }
    ivi_set(w, -1, 0);
    a->visible = 0;
    fprintf(stderr, "[cs] hidden\n");
}

/* ------------------------------------------------------------------ */
/* Input: keyboard (knob/buttons), touch, pointer -> app.c            */
/* ------------------------------------------------------------------ */
static void kbd_keymap(void *d, struct wl_proxy *k, uint32_t fmt, int32_t fd, uint32_t size)
{ (void)d; (void)k; (void)fmt; (void)size; close(fd); }
static void kbd_enter(void *d, struct wl_proxy *k, uint32_t serial, struct wl_proxy *s, void *keys)
{ (void)d; (void)k; (void)serial; (void)s; (void)keys; fprintf(stderr, "[cs] keyboard focus: in\n"); }
static void kbd_leave(void *data, struct wl_proxy *k, uint32_t serial, struct wl_proxy *s)
{
    (void)k; (void)serial; (void)s;
    app_keyboard_leave(&((struct wl *)data)->app);
    fprintf(stderr, "[cs] keyboard focus: out\n");
}
static void kbd_modifiers(void *d, struct wl_proxy *k, uint32_t serial, uint32_t dep,
                          uint32_t lat, uint32_t lock, uint32_t group)
{ (void)d; (void)k; (void)serial; (void)dep; (void)lat; (void)lock; (void)group; }
/* key is an evdev code (linux/input.h), state: 1 pressed, 0 released. */
static void kbd_key(void *data, struct wl_proxy *k, uint32_t serial, uint32_t time,
                    uint32_t key, uint32_t state)
{
    (void)k; (void)serial; (void)time;
    app_key(&((struct wl *)data)->app, key, state != 0);
}

static const void *keyboard_listener[] = {
    (void *)kbd_keymap, (void *)kbd_enter, (void *)kbd_leave,
    (void *)kbd_key, (void *)kbd_modifiers,
};

/* Coordinates in wl_fixed (24.8), surface-relative = screen (the window is at 0,0). */
static void touch_down(void *data, struct wl_proxy *t, uint32_t serial, uint32_t time,
                       struct wl_proxy *s, int32_t id, int32_t x, int32_t y)
{
    (void)t; (void)serial; (void)time; (void)s;
    app_touch(&((struct wl *)data)->app, TOUCH_DOWN, id, x / 256, y / 256);
}
static void touch_up(void *data, struct wl_proxy *t, uint32_t serial, uint32_t time, int32_t id)
{
    (void)t; (void)serial; (void)time;
    app_touch(&((struct wl *)data)->app, TOUCH_UP, id, 0, 0);
}
static void touch_motion(void *data, struct wl_proxy *t, uint32_t time, int32_t id,
                         int32_t x, int32_t y)
{
    (void)t; (void)time;
    app_touch(&((struct wl *)data)->app, TOUCH_MOTION, id, x / 256, y / 256);
}
static void touch_frame(void *d, struct wl_proxy *t) { (void)d; (void)t; }
static void touch_cancel(void *data, struct wl_proxy *t)
{
    (void)t;
    app_touch_cancel(&((struct wl *)data)->app);
}

static const void *touch_listener[] = {
    (void *)touch_down, (void *)touch_up, (void *)touch_motion,
    (void *)touch_frame, (void *)touch_cancel,
};

/* If the compositor reports the touchscreen as a pointer, handle it the same way. */
static void ptr_enter(void *data, struct wl_proxy *p, uint32_t serial, struct wl_proxy *s,
                      int32_t x, int32_t y)
{
    struct wl *w = data;
    (void)p; (void)serial; (void)s;
    w->ptr_x = x / 256;
    w->ptr_y = y / 256;
}
static void ptr_leave(void *data, struct wl_proxy *p, uint32_t serial, struct wl_proxy *s)
{
    struct wl *w = data;
    (void)p; (void)serial; (void)s;
    if (w->ptr_down)
        app_touch(&w->app, TOUCH_UP, POINTER_ID, 0, 0);
    w->ptr_down = 0;
}
static void ptr_motion(void *data, struct wl_proxy *p, uint32_t time, int32_t x, int32_t y)
{
    struct wl *w = data;
    (void)p; (void)time;
    w->ptr_x = x / 256;
    w->ptr_y = y / 256;
    if (w->ptr_down)
        app_touch(&w->app, TOUCH_MOTION, POINTER_ID, w->ptr_x, w->ptr_y);
}
static void ptr_button(void *data, struct wl_proxy *p, uint32_t serial, uint32_t time,
                       uint32_t button, uint32_t state)
{
    struct wl *w = data;
    (void)p; (void)serial; (void)time; (void)button;
    w->ptr_down = state != 0;
    app_touch(&w->app, state ? TOUCH_DOWN : TOUCH_UP, POINTER_ID, w->ptr_x, w->ptr_y);
}
static void ptr_axis(void *d, struct wl_proxy *p, uint32_t time, uint32_t axis, int32_t v)
{ (void)d; (void)p; (void)time; (void)axis; (void)v; }

static const void *pointer_listener[] = {
    (void *)ptr_enter, (void *)ptr_leave, (void *)ptr_motion,
    (void *)ptr_button, (void *)ptr_axis,
};

static void seat_capabilities(void *data, struct wl_proxy *seat, uint32_t caps)
{
    struct wl *w = data;

    fprintf(stderr, "[cs] seat:%s%s%s\n", caps & WL_SEAT_CAP_POINTER ? " pointer" : "",
            caps & WL_SEAT_CAP_KEYBOARD ? " keyboard" : "", caps & WL_SEAT_CAP_TOUCH ? " touch" : "");
    if ((caps & WL_SEAT_CAP_KEYBOARD) && !w->keyboard) {
        w->keyboard = wl_proxy_marshal_constructor(seat, WL_SEAT_GET_KEYBOARD,
                                                   wl_keyboard_interface, NULL);
        wl_proxy_add_listener(w->keyboard, (void (**)(void))keyboard_listener, w);
    }
    if ((caps & WL_SEAT_CAP_TOUCH) && !w->touch) {
        w->touch = wl_proxy_marshal_constructor(seat, WL_SEAT_GET_TOUCH,
                                                wl_touch_interface, NULL);
        wl_proxy_add_listener(w->touch, (void (**)(void))touch_listener, w);
    }
    if ((caps & WL_SEAT_CAP_POINTER) && !w->pointer) {
        w->pointer = wl_proxy_marshal_constructor(seat, WL_SEAT_GET_POINTER,
                                                  wl_pointer_interface, NULL);
        wl_proxy_add_listener(w->pointer, (void (**)(void))pointer_listener, w);
    }
}
static void seat_name(void *d, struct wl_proxy *s, const char *n) { (void)d; (void)s; (void)n; }

static const void *seat_listener[] = { (void *)seat_capabilities, (void *)seat_name };

/* ------------------------------------------------------------------ */
/* Compositor globals                                                 */
/* ------------------------------------------------------------------ */
static struct wl_proxy *registry_bind(struct wl_proxy *registry, uint32_t name,
                                      const struct wl_interface *iface, uint32_t version)
{
    if (version > (uint32_t)iface->version)
        version = (uint32_t)iface->version;
    return wl_proxy_marshal_constructor(registry, WL_REGISTRY_BIND, iface,
                                        name, iface->name, version, NULL);
}

static void registry_global(void *data, struct wl_proxy *registry, uint32_t name,
                            const char *iface, uint32_t version)
{
    struct wl *w = data;

    if (!strcmp(iface, "wl_compositor"))
        w->compositor = registry_bind(registry, name, wl_compositor_interface, version);
    else if (!strcmp(iface, "wl_shm"))
        w->shm = registry_bind(registry, name, wl_shm_interface, version);
    else if (!strcmp(iface, "wl_ivi"))
        w->ivi = registry_bind(registry, name, wl_ivi_interface, version);
    else if (!strcmp(iface, "wl_ivi_shell"))
        w->ivi_shell = registry_bind(registry, name, wl_ivi_shell_interface, version);
    else if (!strcmp(iface, "wl_seat") && !w->seat) {
        w->seat = registry_bind(registry, name, wl_seat_interface, version);
        wl_proxy_add_listener(w->seat, (void (**)(void))seat_listener, w);
    }
}

static void registry_global_remove(void *d, struct wl_proxy *r, uint32_t name)
{ (void)d; (void)r; (void)name; }

static const void *registry_listener[] = {
    (void *)registry_global, (void *)registry_global_remove,
};

/* ------------------------------------------------------------------ */
/* Window: two shm buffers + wl_surface + IVI registration            */
/* ------------------------------------------------------------------ */
static int create_window(struct wl *w)
{
    char tmpl[] = "/tmp/custom_surface_shm_XXXXXX";
    struct wl_proxy *pool;
    uint32_t *px;
    int i, fd = mkstemp(tmpl);

    if (fd < 0)
        return -1;
    unlink(tmpl);
    if (ftruncate(fd, (off_t)(2 * FRAME_BYTES)) < 0) {
        close(fd);
        return -1;
    }
    px = mmap(NULL, 2 * FRAME_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return -1;
    }
    pool = wl_proxy_marshal_constructor(w->shm, WL_SHM_CREATE_POOL, wl_shm_pool_interface,
                                        NULL, fd, (int32_t)(2 * FRAME_BYTES));
    close(fd);
    if (!pool)
        return -1;
    for (i = 0; i < 2; i++) {
        w->bufs[i].px = px + (size_t)i * WIDTH * HEIGHT;
        w->bufs[i].wl = wl_proxy_marshal_constructor(pool, WL_SHM_POOL_CREATE_BUFFER,
                wl_buffer_interface, NULL, (int32_t)(i * FRAME_BYTES), WIDTH, HEIGHT, STRIDE,
                (uint32_t)WL_SHM_FORMAT_XRGB8888);
        if (!w->bufs[i].wl)
            return -1;
        wl_proxy_add_listener(w->bufs[i].wl, (void (**)(void))buffer_listener, &w->bufs[i]);
        /* XRGB8888 little-endian == CAIRO_FORMAT_RGB24 */
        w->bufs[i].cs = cairo_image_surface_create_for_data((unsigned char *)w->bufs[i].px,
                CAIRO_FORMAT_RGB24, WIDTH, HEIGHT, STRIDE);
    }

    w->surface = wl_proxy_marshal_constructor(w->compositor, WL_COMPOSITOR_CREATE_SURFACE,
                                              wl_surface_interface, NULL);
    if (!w->surface)
        return -1;

    /* Register under the IVI name. The compositor creates it hidden
     * (compositor.conf: initial-visibility = hide). */
    wl_proxy_marshal(w->ivi, WL_IVI_REGISTER_SURFACE, w->surface, w->app.name);
    redraw(w);
    return wl_display_roundtrip(w->dpy) < 0 ? -1 : 0;
}

/* ------------------------------------------------------------------ */
static void usage(const char *prog)
{
    fprintf(stderr,
        "usage: %s [-d] [-z ZORDER] [-a ALPHA] [-k] [-H] [-t SECONDS]\n"
        "  -d          demo / input test: shows buttons and touches, EXIT button (implies -k)\n"
        "  -z ZORDER   z-order when shown (default 1000, above HMI)\n"
        "  -a ALPHA    alpha when shown, 0..255 (default 255)\n"
        "  -k          grab keyboard focus while shown, give it back to " HMI_SURFACE "\n"
        "  -H          start hidden (show later with SIGUSR1)\n"
        "  -t SECONDS  hide and exit after N seconds\n"
        "env WAYLAND_IVI_SURFACE_ID=name (default MY_CUSTOM_SURFACE), XDG_RUNTIME_DIR (default /tmp)\n"
        "signals: USR1 show, USR2 hide, TERM/INT hide+exit\n", prog);
}

int main(int argc, char **argv)
{
    static struct wl w;
    struct app *a = &w.app;
    struct wl_proxy *registry;
    struct sigaction sa;
    struct pollfd pfd;
    int opt, timeout_s = 0, start_hidden = 0, lost = 0;

    w.zorder = 1000;
    w.alpha = 255;
    a->name = getenv("WAYLAND_IVI_SURFACE_ID");
    if (!a->name || !a->name[0])
        a->name = "MY_CUSTOM_SURFACE";

    while ((opt = getopt(argc, argv, "dz:a:kHt:h")) != -1) {
        switch (opt) {
        case 'd': a->demo_mode = 1; a->grab_keyboard = 1; break;
        case 'z': w.zorder = atoi(optarg); break;
        case 'a': w.alpha = atoi(optarg); break;
        case 'k': a->grab_keyboard = 1; break;
        case 'H': start_hidden = 1; break;
        case 't': timeout_s = atoi(optarg); break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 1;
        }
    }
    if (w.alpha < 0) w.alpha = 0;
    if (w.alpha > 255) w.alpha = 255;
    if (w.zorder < 0) w.zorder = 0;

    /* No SA_RESTART, so that a signal interrupts poll(). SIGHUP is ignored —
     * the process survives the terminal session closing. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGALRM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    signal(SIGHUP, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);

    /* The compositor socket (wayland-0) lives in /tmp. */
    setenv("XDG_RUNTIME_DIR", "/tmp", 0);
    w.dpy = wl_display_connect(NULL);
    if (!w.dpy) {
        fprintf(stderr, "[cs] cannot connect to compositor (XDG_RUNTIME_DIR=%s)\n",
                getenv("XDG_RUNTIME_DIR"));
        return 1;
    }
    registry = wl_proxy_marshal_constructor((struct wl_proxy *)w.dpy,
                                            WL_DISPLAY_GET_REGISTRY, wl_registry_interface, NULL);
    wl_proxy_add_listener(registry, (void (**)(void))registry_listener, &w);
    wl_display_roundtrip(w.dpy);   /* globals */
    wl_display_roundtrip(w.dpy);   /* wl_seat capabilities */
    if (!w.compositor || !w.shm || !w.ivi || !w.ivi_shell) {
        fprintf(stderr, "[cs] missing globals: compositor=%d shm=%d wl_ivi=%d wl_ivi_shell=%d\n",
                !!w.compositor, !!w.shm, !!w.ivi, !!w.ivi_shell);
        return 1;
    }
    if (a->grab_keyboard && !w.seat)
        fprintf(stderr, "[cs] WARN: no wl_seat, keyboard grab ignored\n");

    app_init(a);
    if (create_window(&w) < 0) {
        fprintf(stderr, "[cs] cannot create window: %s\n", strerror(errno));
        return 1;
    }
    fprintf(stderr, "[cs] window '%s' %dx%d created (pid %d)%s\n", a->name, WIDTH, HEIGHT,
            (int)getpid(), a->demo_mode ? " [demo]" : "");

    if (!start_hidden)
        win_show(&w);
    if (timeout_s > 0) {
        alarm((unsigned)timeout_s);
        fprintf(stderr, "[cs] will exit in %d s\n", timeout_s);
    }

    /* Own loop instead of wl_display_dispatch(): in this libwayland it restarts
     * poll() on EINTR, so signals don't interrupt it. The poll timeout drives
     * redraws (~30 fps), the hold/idle checks, and covers a signal arriving
     * right before poll(). */
    pfd.fd = wl_display_get_fd(w.dpy);
    pfd.events = POLLIN;
    while (g_running && !a->exit_requested) {
        long long now = now_ms();
        int wait_ms;

        if (g_cmd != CMD_NONE) {
            if (g_cmd == CMD_SHOW)
                win_show(&w);
            else
                win_hide(&w);
            g_cmd = CMD_NONE;
        }
        wait_ms = app_tick(a, now);
        if (a->exit_requested)
            break;
        if (a->dirty) {
            if (now - w.last_draw >= 33)
                redraw(&w);
            if (a->dirty)
                wait_ms = 33;
        }

        while (wl_display_prepare_read(w.dpy) != 0)
            wl_display_dispatch_pending(w.dpy);
        wl_display_flush(w.dpy);
        if (poll(&pfd, 1, wait_ms) < 0) {
            wl_display_cancel_read(w.dpy);
            if (errno == EINTR)
                continue;
            break;
        }
        if (wl_display_read_events(w.dpy) < 0) {
            fprintf(stderr, "[cs] compositor connection lost\n");
            lost = 1;
            break;
        }
        wl_display_dispatch_pending(w.dpy);
    }

    if (!lost) {
        win_hide(&w);                 /* give the focus back to the HMI and hide */
        wl_display_roundtrip(w.dpy);
    }
    wl_display_disconnect(w.dpy);
    fprintf(stderr, "[cs] exit\n");
    return 0;
}
