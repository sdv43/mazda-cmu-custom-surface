/*
 * app.c — the application: drawing (cairo + FreeType) and input logic.
 * Use only the cairo API <= 1.10 — the car has cairo 1.10.2.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/time.h>
#include "app.h"

/* gettimeofday rather than clock_gettime: in the car's glibc 2.11 it is in GLIBC_2.4. */
long long now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (long long)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

/* ------------------------------------------------------------------ */
/* Graphics: the firmware's cairo 1.10 + FreeType, HMI font Tipperary */
/* ------------------------------------------------------------------ */
/* FreeType is declared by hand (2 functions are needed) to avoid its headers. */
typedef struct FT_LibraryRec_ *FT_Library;
typedef struct FT_FaceRec_ *FT_Face;
int FT_Init_FreeType(FT_Library *library);
int FT_New_Face(FT_Library library, const char *path, long face_index, FT_Face *face);
cairo_font_face_t *cairo_ft_font_face_create_for_ft_face(FT_Face face, int load_flags);

/* HMI fonts on the car. */
#ifndef FONT_DIR
#define FONT_DIR "/jci/fonts"
#endif

/* Colors 0xRRGGBB */
#define C_BG_TOP    0x161B21
#define C_BG_BOTTOM 0x0A0C0F
#define C_PANEL     0x12171C
#define C_LINE      0x2A323B
#define C_TEXT      0xF2F2F2
#define C_DIM       0x8A949E
#define C_RED       0xC4161C        /* Mazda accent */
#define C_BLUE      0x2F6FAE
#define C_GREEN     0x5CC872
#define C_YELLOW    0xFFD23F
#define C_CYAN      0x4FC3F7

static cairo_font_face_t *load_font(FT_Library ft, const char *file)
{
    FT_Face face;

    if (ft && FT_New_Face(ft, file, 0, &face) == 0)
        return cairo_ft_font_face_create_for_ft_face(face, 0);   /* FT_Face lives until exit */
    fprintf(stderr, "[cs] WARN: font %s not loaded, using cairo built-in\n", file);
    return cairo_toy_font_face_create("sans", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
}

static void gfx_init(struct app *a)
{
    FT_Library ft = NULL;

    if (FT_Init_FreeType(&ft) != 0)
        ft = NULL;
    a->g.font = load_font(ft, FONT_DIR "/Tipperary.ttf");
    a->g.font_bold = load_font(ft, FONT_DIR "/TipperaryBold.ttf");
}

static void set_color(cairo_t *cr, uint32_t rgb, double alpha)
{
    cairo_set_source_rgba(cr, ((rgb >> 16) & 0xFF) / 255.0, ((rgb >> 8) & 0xFF) / 255.0,
                          (rgb & 0xFF) / 255.0, alpha);
}

static void rounded_rect(cairo_t *cr, double x, double y, double w, double h, double r)
{
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2, 0);
    cairo_arc(cr, x + w - r, y + h - r, r, 0, M_PI / 2);
    cairo_arc(cr, x + r, y + h - r, r, M_PI / 2, M_PI);
    cairo_arc(cr, x + r, y + r, r, M_PI, 3 * M_PI / 2);
    cairo_close_path(cr);
}

static void vgradient(cairo_t *cr, double y0, double y1, uint32_t top, uint32_t bottom)
{
    cairo_pattern_t *p = cairo_pattern_create_linear(0, y0, 0, y1);
    cairo_pattern_add_color_stop_rgb(p, 0, ((top >> 16) & 0xFF) / 255.0,
                                     ((top >> 8) & 0xFF) / 255.0, (top & 0xFF) / 255.0);
    cairo_pattern_add_color_stop_rgb(p, 1, ((bottom >> 16) & 0xFF) / 255.0,
                                     ((bottom >> 8) & 0xFF) / 255.0, (bottom & 0xFF) / 255.0);
    cairo_set_source(cr, p);
    cairo_pattern_destroy(p);
}

static void font(cairo_t *cr, cairo_font_face_t *face, double size)
{
    cairo_set_font_face(cr, face);
    cairo_set_font_size(cr, size);
}

/* UTF-8 text; (x, y) is the left edge and the baseline. */
static void label(cairo_t *cr, double x, double y, uint32_t rgb, const char *s)
{
    set_color(cr, rgb, 1);
    cairo_move_to(cr, x, y);
    cairo_show_text(cr, s);
}

/* Text centered in a rectangle. */
static void label_center(cairo_t *cr, double x, double y, double w, double h,
                         uint32_t rgb, const char *s)
{
    cairo_text_extents_t e;
    cairo_text_extents(cr, s, &e);
    label(cr, x + (w - e.width) / 2 - e.x_bearing, y + (h - e.height) / 2 - e.y_bearing, rgb, s);
}

/* Normal mode: the window content — replace with your own. */
static void draw_static(cairo_t *cr, const struct app *a)
{
    char line[96];

    vgradient(cr, 0, HEIGHT, C_BG_TOP, C_BG_BOTTOM);
    cairo_paint(cr);
    set_color(cr, C_RED, 1);
    cairo_rectangle(cr, 40, 150, 120, 4);
    cairo_fill(cr);

    font(cr, a->g.font_bold, 48);
    label(cr, 40, 120, C_TEXT, "Custom window");
    font(cr, a->g.font, 22);
    label(cr, 40, 200, C_DIM, "Mazda CMU · Wayland + IVI · cairo");
    snprintf(line, sizeof(line), "Surface: %s", a->name);
    label(cr, 40, 250, C_TEXT, line);
    label(cr, 40, 285, a->grab_keyboard ? C_GREEN : C_DIM,
          a->grab_keyboard ? "Keyboard: this window (-k)" : "Keyboard: HMI");
    font(cr, a->g.font, 18);
    label(cr, 40, 440, C_DIM, "app.c — draw_static()");
}

/* ------------------------------------------------------------------ */
/* Demo mode (-d): keys, touch and exit test                          */
/* ------------------------------------------------------------------ */
enum { BTN_NONE, BTN_EXIT, BTN_CLEAR };

static const struct { int id, x, y, w, h; const char *label; uint32_t color; } demo_btns[] = {
    { BTN_CLEAR, 456, 10, 160, 54, "Clear", C_BLUE },
    { BTN_EXIT,  628, 10, 160, 54, "Exit",  C_RED  },
};
#define N_DEMO_BTNS ((int)(sizeof(demo_btns) / sizeof(demo_btns[0])))

static int demo_btn_at(int x, int y)
{
    int i;
    for (i = 0; i < N_DEMO_BTNS; i++)
        if (x >= demo_btns[i].x && x < demo_btns[i].x + demo_btns[i].w &&
            y >= demo_btns[i].y && y < demo_btns[i].y + demo_btns[i].h)
            return demo_btns[i].id;
    return BTN_NONE;
}

/* Targets for checking touch accuracy (centers). */
static const int demo_targets[][2] = {
    { 340, 110 }, { 770, 110 }, { 555, 270 }, { 340, 410 }, { 770, 410 },
};

static const uint32_t touch_colors[MAX_TOUCH] = {
    C_YELLOW, C_CYAN, 0xFF6E6E, 0x81C784, 0xBA68C8,
    0xFFB74D, 0x4DD0E1, 0xF06292, 0xAED581, 0x9575CD,
};

/* Name of an evdev code (linux/input.h). */
static const char *key_name(uint32_t code, char *buf, size_t n)
{
    static const char *row1 = "QWERTYUIOP", *row2 = "ASDFGHJKL", *row3 = "ZXCVBNM";
    switch (code) {
    case 1:   return "KEY_ESC";
    case 14:  return "KEY_BACKSPACE";
    case 15:  return "KEY_TAB";
    case 26:  return "KEY_LEFTBRACE";
    case 27:  return "KEY_RIGHTBRACE";
    case 28:  return "KEY_ENTER";
    case 57:  return "KEY_SPACE";
    case 102: return "KEY_HOME";
    case 103: return "KEY_UP";
    case 105: return "KEY_LEFT";
    case 106: return "KEY_RIGHT";
    case 108: return "KEY_DOWN";
    case 113: return "KEY_MUTE";
    case 114: return "KEY_VOLUMEDOWN";
    case 115: return "KEY_VOLUMEUP";
    }
    if (code >= 2 && code <= 11)
        snprintf(buf, n, "KEY_%c", code == 11 ? '0' : (char)('0' + code - 1));
    else if (code >= 16 && code <= 25)
        snprintf(buf, n, "KEY_%c", row1[code - 16]);
    else if (code >= 30 && code <= 38)
        snprintf(buf, n, "KEY_%c", row2[code - 30]);
    else if (code >= 44 && code <= 50)
        snprintf(buf, n, "KEY_%c", row3[code - 44]);
    else
        snprintf(buf, n, "KEY_%u", code);
    return buf;
}

/* What the key is on the Mazda control panel ("?" — not confirmed). */
static const char *key_role(uint32_t code)
{
    switch (code) {
    case 49: case 50: return "knob rotation";
    case 103: return "knob tilt up";
    case 108: return "knob tilt down";
    case 105: return "knob tilt left";
    case 106: return "knob tilt right";
    case 28:  return "knob press ?";
    case 14:  return "back ?";
    case 102: return "home ?";
    case 18: case 19: case 20: return "shortcut button";
    }
    return "";
}

/* Static part of the demo screen — drawn once into a->g.bg. */
static void draw_demo_background(cairo_t *cr, const struct app *a)
{
    int i, x, y;

    vgradient(cr, 0, HEIGHT, C_BG_TOP, C_BG_BOTTOM);
    cairo_paint(cr);

    /* 40 px grid */
    set_color(cr, 0xFFFFFF, 0.05);
    for (x = 320; x < WIDTH; x += 40)
        cairo_rectangle(cr, x, 74, 1, 364);
    for (y = 110; y < 438; y += 40)
        cairo_rectangle(cr, 300, y, WIDTH - 300, 1);
    cairo_fill(cr);

    /* targets */
    cairo_set_line_width(cr, 2);
    set_color(cr, 0x6A7580, 1);
    for (i = 0; i < (int)(sizeof(demo_targets) / sizeof(demo_targets[0])); i++) {
        double tx = demo_targets[i][0] + 0.5, ty = demo_targets[i][1] + 0.5;
        cairo_new_sub_path(cr);
        cairo_arc(cr, tx, ty, 18, 0, 2 * M_PI);
        cairo_move_to(cr, tx - 26, ty);
        cairo_line_to(cr, tx + 26, ty);
        cairo_move_to(cr, tx, ty - 26);
        cairo_line_to(cr, tx, ty + 26);
    }
    cairo_stroke(cr);

    /* top and bottom bars */
    vgradient(cr, 0, 72, 0x1E252D, 0x13181D);
    cairo_rectangle(cr, 0, 0, WIDTH, 72);
    cairo_fill(cr);
    set_color(cr, C_RED, 1);
    cairo_rectangle(cr, 0, 72, WIDTH, 2);
    cairo_fill(cr);
    set_color(cr, 0x0F1317, 1);
    cairo_rectangle(cr, 0, 438, WIDTH, 42);
    cairo_fill(cr);
    set_color(cr, C_LINE, 1);
    cairo_rectangle(cr, 0, 438, WIDTH, 1);
    cairo_fill(cr);

    /* left panel */
    rounded_rect(cr, 10, 84, 280, 344, 12);
    set_color(cr, C_PANEL, 0.95);
    cairo_fill_preserve(cr);
    set_color(cr, C_LINE, 1);
    cairo_set_line_width(cr, 1);
    cairo_stroke(cr);

    font(cr, a->g.font_bold, 14);
    label(cr, 24, 108, C_DIM, "LAST KEY");
    label(cr, 24, 264, C_DIM, "HISTORY");
    font(cr, a->g.font, 14);
    label(cr, 14, 474, C_DIM,
          "Exit: the Exit button or hold any key for 3 s. Exits by itself after 2 min without input.");
}

static void draw_demo_button(cairo_t *cr, const struct app *a, int i)
{
    int pressed = a->d.pressed_btn == demo_btns[i].id;
    double x = demo_btns[i].x, y = demo_btns[i].y, w = demo_btns[i].w, h = demo_btns[i].h;
    uint32_t c = demo_btns[i].color;

    rounded_rect(cr, x, y, w, h, 10);
    if (pressed) {
        set_color(cr, 0xFFFFFF, 1);
    } else {
        cairo_pattern_t *p = cairo_pattern_create_linear(0, y, 0, y + h);
        double r = ((c >> 16) & 0xFF) / 255.0, g = ((c >> 8) & 0xFF) / 255.0, b = (c & 0xFF) / 255.0;
        cairo_pattern_add_color_stop_rgb(p, 0, r * 1.25 > 1 ? 1 : r * 1.25,
                                         g * 1.25 > 1 ? 1 : g * 1.25, b * 1.25 > 1 ? 1 : b * 1.25);
        cairo_pattern_add_color_stop_rgb(p, 1, r * 0.75, g * 0.75, b * 0.75);
        cairo_set_source(cr, p);
        cairo_pattern_destroy(p);
    }
    cairo_fill(cr);
    font(cr, a->g.font_bold, 22);
    label_center(cr, x, y, w, h, pressed ? c : C_TEXT, demo_btns[i].label);
}

static void draw_demo(cairo_t *cr, const struct app *a)
{
    const struct demo *d = &a->d;
    char line[128], nb[16];
    int i;

    cairo_set_source_surface(cr, a->g.bg, 0, 0);
    cairo_paint(cr);
    cairo_set_source_surface(cr, a->g.trail, 0, 0);
    cairo_paint(cr);

    /* top bar */
    font(cr, a->g.font_bold, 28);
    label(cr, 18, 38, C_TEXT, "Input test");
    font(cr, a->g.font, 16);
    label(cr, 18, 62, a->focused ? C_GREEN : 0xE57373,
          a->focused ? "keyboard: this window" : "keyboard: HMI");
    for (i = 0; i < N_DEMO_BTNS; i++)
        draw_demo_button(cr, a, i);

    /* left panel: keys */
    if (d->key_events) {
        uint32_t c = d->last_key_down ? C_YELLOW : C_TEXT;
        font(cr, a->g.font_bold, 30);
        label(cr, 24, 146, c, key_name(d->last_key, nb, sizeof(nb)));
        font(cr, a->g.font, 17);
        label(cr, 24, 172, C_DIM, key_role(d->last_key));
        snprintf(line, sizeof(line), "code %u · %s", d->last_key,
                 d->last_key_down ? "down" : "up");
        label(cr, 24, 198, c, line);
    } else {
        font(cr, a->g.font, 20);
        label(cr, 24, 146, C_DIM, "press a key");
    }
    /* knob rotation dial: 49 -> -1, 50 -> +1, 15° per step */
    {
        double cx = 248, cy = 224, ang = d->knob * M_PI / 12 - M_PI / 2;
        cairo_set_line_width(cr, 2);
        set_color(cr, C_LINE, 1);
        cairo_new_sub_path(cr);
        cairo_arc(cr, cx, cy, 22, 0, 2 * M_PI);
        cairo_stroke(cr);
        set_color(cr, C_CYAN, 1);
        cairo_set_line_width(cr, 4);
        cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
        cairo_move_to(cr, cx, cy);
        cairo_line_to(cr, cx + 17 * cos(ang), cy + 17 * sin(ang));
        cairo_stroke(cr);
        font(cr, a->g.font, 17);
        snprintf(line, sizeof(line), "knob: %+d", d->knob);
        label(cr, 24, 230, C_CYAN, line);
    }
    font(cr, a->g.font, 16);
    for (i = 0; i < d->hist_n; i++)
        label(cr, 24, 290 + i * 22, i == 0 ? C_TEXT : C_DIM, d->hist[i]);

    /* touches: translucent circle + ring + crosshair + coordinates */
    for (i = 0; i < MAX_TOUCH; i++) {
        cairo_text_extents_t e;
        double x, y, lx, ly;

        if (!d->t[i].down)
            continue;
        x = d->t[i].x + 0.5;
        y = d->t[i].y + 0.5;
        cairo_new_sub_path(cr);
        cairo_arc(cr, x, y, 36, 0, 2 * M_PI);
        set_color(cr, touch_colors[i], 0.22);
        cairo_fill_preserve(cr);
        set_color(cr, touch_colors[i], 1);
        cairo_set_line_width(cr, 3);
        cairo_stroke(cr);
        cairo_set_line_width(cr, 1);
        cairo_move_to(cr, x - 50, y);
        cairo_line_to(cr, x + 50, y);
        cairo_move_to(cr, x, y - 50);
        cairo_line_to(cr, x, y + 50);
        cairo_stroke(cr);

        font(cr, a->g.font_bold, 16);
        snprintf(line, sizeof(line), "%d, %d", d->t[i].x, d->t[i].y);
        cairo_text_extents(cr, line, &e);
        lx = x + 44 + e.width + 12 > WIDTH ? x - 44 - e.width - 12 : x + 44;
        ly = y - 44 < 80 ? y + 44 : y - 44 - 24;
        rounded_rect(cr, lx, ly, e.width + 12, 24, 6);
        set_color(cr, 0x000000, 0.7);
        cairo_fill(cr);
        label(cr, lx + 6, ly + 17, touch_colors[i], line);
    }

    /* bottom bar */
    font(cr, a->g.font, 16);
    if (d->have_last)
        snprintf(line, sizeof(line), "touch id %d: %d, %d   ·   touches %u   ·   keys %u"
                 "   ·   frame %d ms", d->last_id, d->last_x, d->last_y,
                 d->touch_events, d->key_events, a->g.frame_ms);
    else
        snprintf(line, sizeof(line), "touch the screen   ·   touches %u   ·   keys %u"
                 "   ·   frame %d ms", d->touch_events, d->key_events, a->g.frame_ms);
    label(cr, 14, 456, C_TEXT, line);
}

static void demo_init(struct app *a)
{
    cairo_t *cr;

    a->g.bg = cairo_image_surface_create(CAIRO_FORMAT_RGB24, WIDTH, HEIGHT);
    cr = cairo_create(a->g.bg);
    draw_demo_background(cr, a);
    cairo_destroy(cr);

    a->g.trail = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, WIDTH, HEIGHT);
    a->g.trail_cr = cairo_create(a->g.trail);
    cairo_set_line_cap(a->g.trail_cr, CAIRO_LINE_CAP_ROUND);
    cairo_set_line_join(a->g.trail_cr, CAIRO_LINE_JOIN_ROUND);
    cairo_set_line_width(a->g.trail_cr, 6);
}

static void demo_trail_clear(struct app *a)
{
    cairo_save(a->g.trail_cr);
    cairo_set_operator(a->g.trail_cr, CAIRO_OPERATOR_CLEAR);
    cairo_paint(a->g.trail_cr);
    cairo_restore(a->g.trail_cr);
}

/* The finger trail is drawn straight into the a->g.trail layer (accumulates). */
static void demo_trail(struct app *a, int slot, int x0, int y0, int x1, int y1)
{
    cairo_t *cr = a->g.trail_cr;
    set_color(cr, touch_colors[slot], 0.85);
    cairo_move_to(cr, x0 + 0.5, y0 + 0.5);
    cairo_line_to(cr, x1 + 0.5, y1 + 0.5);
    cairo_stroke(cr);
}

static void demo_history(struct app *a, uint32_t key, int down)
{
    struct demo *d = &a->d;
    char nb[16];

    memmove(d->hist[1], d->hist[0], sizeof(d->hist[0]) * (HIST_MAX - 1));
    snprintf(d->hist[0], sizeof(d->hist[0]), "%u  %s  %s", key,
             key_name(key, nb, sizeof(nb)), down ? "down" : "up");
    if (d->hist_n < HIST_MAX)
        d->hist_n++;
}

/* A finger. kind: TOUCH_DOWN / _MOTION / _UP. */
void app_touch(struct app *a, int kind, int id, int x, int y)
{
    struct demo *d = &a->d;
    int slot = (id < 0 ? -id : id) % MAX_TOUCH;

    if (kind == TOUCH_DOWN)
        fprintf(stderr, "[cs] touch down id=%d x=%d y=%d\n", id, x, y);
    else if (kind == TOUCH_UP)
        fprintf(stderr, "[cs] touch up id=%d\n", id);
    if (!a->demo_mode)
        return;
    d->last_input = now_ms();
    if (kind == TOUCH_DOWN) {
        d->touch_events++;
        d->t[slot].down = 1;
        d->t[slot].x = x;
        d->t[slot].y = y;
        d->pressed_btn = demo_btn_at(x, y);
        d->pressed_id = id;
    }
    if (kind != TOUCH_UP) {
        if (!d->t[slot].down)
            return;
        if (d->pressed_btn == BTN_NONE)
            demo_trail(a, slot, d->t[slot].x, d->t[slot].y, x, y);
        else if (d->pressed_id == id && demo_btn_at(x, y) != d->pressed_btn)
            d->pressed_btn = BTN_NONE;                 /* finger slid off the button */
        d->t[slot].x = x;
        d->t[slot].y = y;
        d->last_x = x;
        d->last_y = y;
        d->last_id = id;
        d->have_last = 1;
    } else {
        d->t[slot].down = 0;
        if (d->pressed_id == id && d->pressed_btn != BTN_NONE) {
            if (d->pressed_btn == BTN_EXIT) {
                fprintf(stderr, "[cs] EXIT button\n");
                a->exit_requested = 1;
            } else if (d->pressed_btn == BTN_CLEAR) {
                demo_trail_clear(a);
                d->hist_n = 0;
                d->knob = 0;
            }
            d->pressed_btn = BTN_NONE;
        }
    }
    a->dirty = 1;
}


/* ------------------------------------------------------------------ */
/* Interface for the platform layer (app.h)                           */
/* ------------------------------------------------------------------ */
void app_init(struct app *a)
{
    gfx_init(a);
    if (a->demo_mode)
        demo_init(a);
    a->d.last_input = now_ms();
    a->dirty = 1;
}

void app_draw(cairo_t *cr, struct app *a)
{
    long long t0 = now_ms();

    if (a->demo_mode)
        draw_demo(cr, a);
    else
        draw_static(cr, a);
    a->g.frame_ms = (int)(now_ms() - t0);
    a->dirty = 0;
}

/* A control-panel key: evdev code (linux/input.h), down: 1 pressed, 0 released. */
void app_key(struct app *a, uint32_t key, int down)
{
    struct demo *d = &a->d;

    fprintf(stderr, "[cs] key code=%u %s\n", key, down ? "down" : "up");
    if (!a->demo_mode)
        return;
    d->last_input = now_ms();
    d->last_key = key;
    d->last_key_down = down != 0;
    demo_history(a, key, down != 0);
    if (down) {
        d->key_events++;
        d->held_key = key;
        d->held_since = d->last_input;
        if (key == 50) d->knob++;
        if (key == 49) d->knob--;
    } else if (d->held_key == key) {
        d->held_key = 0;
    }
    a->dirty = 1;
}

void app_touch_cancel(struct app *a)
{
    memset(a->d.t, 0, sizeof(a->d.t));
    a->d.pressed_btn = BTN_NONE;
    a->dirty = 1;
}

void app_keyboard_leave(struct app *a)
{
    a->d.held_key = 0;
}

int app_tick(struct app *a, long long now)
{
    if (!a->demo_mode || !a->visible)
        return 500;
    if (a->d.held_key && now - a->d.held_since >= HOLD_EXIT_MS) {
        fprintf(stderr, "[cs] key %u held 3 s -> exit\n", a->d.held_key);
        a->exit_requested = 1;
    } else if (now - a->d.last_input >= IDLE_EXIT_MS) {
        fprintf(stderr, "[cs] idle 120 s -> exit\n");
        a->exit_requested = 1;
    }
    return a->d.held_key ? 100 : 500;
}
