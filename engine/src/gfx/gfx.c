/* gfx/gfx.c — module lifecycle, shake (GFX §5), screen effects (GFX §6), frame update and render, save thumbnail.
 * State is static and fixed-size; the shake table is copied from Gameexe on start. gfx_shutdown releases every
 * byte the module owns (objects, images). */
#include "gfx/gfx.h"
#include "core/log.h"
#include "core/str.h"
#include "fmt/gameexe.h"
#include "gfx/pixel.h"
#include "gfx/render.h"
#include <limits.h>
#include <string.h>

static bool g_inited;
static const Canvas *g_last_fb;   /* owned by the caller of gfx_render; read by load wipes and captures */
static uint32_t g_scene_px[SCREEN_W * SCREEN_H];
static Canvas g_scene = { g_scene_px, SCREEN_W, SCREEN_H, SCREEN_W };   /* the picture before a shake offset */

/* ------------------------------------------------------------ shake (GFX §5) */

enum { SHAKE_MAX_STEPS = 64, SHAKE_FIELDS = 3 };
typedef struct ShakeStep { int dx, dy, ms; } ShakeStep;
static struct { int no, t, nstep; ShakeStep step[SHAKE_MAX_STEPS]; } g_shake = { .no = -1 };

/* Gameexe.ini:1026-1033, used when gx_tuples has no data (WP1 day-0 stub) */
static const char *const SHAKE_FALLBACK[] = {
    "(000,032,100)(032,032,100)(032,000,100)(032,-32,100)(000,-32,100)(-32,-32,100)(-32,000,100)(-32,032,100)",
    "(000,016,016)(000,000,016)(000,016,016)",
    "(006,032,032)(000,000,032)(-04,032,032)",
    "(000,016,016)(000,000,016)(000,016,016)(000,000,016)(000,016,016)",
    "(080,005,080)(000,000,016)(-016,001,016)(000,000,016)",
    "(-15,050,050)(000,000,050)(010,050,050)",
    "(010,050,050)(000,000,050)(-06,050,050)",
    "(032,006,032)(000,000,032)(032,-04,032)",
};

/* Every integer of a "(a,b,c)(d,e,f)" string, at most max. */
static int scan_ints(const char *text, int *out, int max)
{
    Str s = str_c(text);
    int n = 0;
    for (size_t pos = 0; pos < s.n && n < max;) {
        int32_t v;
        if (str_scan_i32(s, &pos, &v)) out[n++] = v;
        else pos++;
    }
    return n;
}

static int shake_load(int no)
{
    char key[32];
    int buf[SHAKE_MAX_STEPS * SHAKE_FIELDS];
    if (!str_fmt(key, sizeof key, "SHAKE.%03d", no)) return 0;
    int n = gx_tuples(key, buf, (int)COUNT_OF(buf));
    if (n <= 0 && no >= 0 && no < (int)COUNT_OF(SHAKE_FALLBACK))
        n = scan_ints(SHAKE_FALLBACK[no], buf, (int)COUNT_OF(buf));
    n = n < 0 ? 0 : n > (int)COUNT_OF(buf) ? (int)COUNT_OF(buf) : n;
    g_shake.nstep = n / SHAKE_FIELDS;
    for (int k = 0; k < g_shake.nstep; k++) {
        const int *f = &buf[(size_t)k * SHAKE_FIELDS];
        g_shake.step[k] = (ShakeStep){ f[0], f[1], f[2] };
    }
    return g_shake.nstep;
}

bool gfx_shake_start(int no)   /* FUN_005c74b0 */
{
    if (no < 0 || no >= gx_int("SHAKE.CNT", 0, 16)) return false;
    if (shake_load(no) <= 0) return false;
    g_shake.no = no;
    g_shake.t = 0;
    if (log_trace_on("GFX")) log_trace("GFX", "shake %d steps=%d", no, g_shake.nstep);
    return true;
}

bool gfx_shake_active(void) { return g_shake.no >= 0; }
void gfx_shake_stop(void) { g_shake.no = -1; }

void gfx_shake_offset(int *dx, int *dy)   /* step function, no interpolation */
{
    *dx = *dy = 0;
    if (g_shake.no < 0) return;
    int64_t acc = 0;
    for (int k = 0; k < g_shake.nstep; k++) {
        const ShakeStep *s = &g_shake.step[k];
        if (g_shake.t >= acc && g_shake.t < acc + s->ms) { *dx = s->dx; *dy = s->dy; return; }
        acc += s->ms;
    }
}

static void shake_frame(int32_t game_dt)
{
    if (g_shake.no < 0) return;
    g_shake.t = core_wrap_add_i32(g_shake.t, game_dt);
    int64_t total = 0;
    for (int k = 0; k < g_shake.nstep; k++) total += g_shake.step[k].ms;
    if (g_shake.t >= total) g_shake.no = -1;
}

/* ------------------------------------------------------------ screen effects (GFX §6) */

static Effect g_effect[GFX_EFFECT_CNT];
static Effect g_screen;   /* screen root props 0/2..8/19..21 (unused by the game) */

void gfx_effect_init(Effect *e)
{
    ievent_init(&e->x, 0); ievent_init(&e->y, 0); ievent_init(&e->z, 0);
    for (int k = 0; k < TONE_CNT; k++) ievent_init(&e->tone[k], 0);
    e->begin_order = e->end_order = 0;              /* FUN_005c6ae0 defaults */
    e->begin_layer = INT_MIN; e->end_layer = INT_MAX;
    e->wipe_copy = e->wipe_erase = false;
}

Effect *gfx_effect(int i) { return (i >= 0 && i < GFX_EFFECT_CNT) ? &g_effect[i] : NULL; }
Effect *gfx_screen_tone(void) { return &g_screen; }

static void effect_frame(Effect *e, int32_t g, int32_t r)
{
    ievent_frame(&e->x, g, r); ievent_frame(&e->y, g, r); ievent_frame(&e->z, g, r);
    for (int k = 0; k < TONE_CNT; k++) ievent_frame(&e->tone[k], g, r);
}

static void effect_apply(Canvas *t, const Effect *e)   /* whole picture in this game */
{
    int v[TONE_CNT];
    for (int k = 0; k < TONE_CNT; k++) v[k] = px_clamp255(e->tone[k].cur);
    Tone tn = { v[TONE_MONO], v[TONE_REVERSE], v[TONE_BRIGHT], v[TONE_DARK], v[TONE_R], v[TONE_G], v[TONE_B],
                v[TONE_RATE], v[TONE_ADD_R], v[TONE_ADD_G], v[TONE_ADD_B] };
    canvas_tone(t, &tn, NULL);
}

/* ------------------------------------------------------------ lifecycle */

void gfx_reset_all(void)
{
    gfx_wipe_reset();
    g_shake.no = -1;
    gfx_objects_reset();
    for (int i = 0; i < GFX_EFFECT_CNT; i++) gfx_effect_init(&g_effect[i]);
    gfx_effect_init(&g_screen);
}

void gfx_init(void)
{
    if (!g_inited) gfx_elm_init();
    g_inited = true;
    gfx_reset_all();
}

void gfx_shutdown(void)
{
    gfx_reset_all();          /* frees every object's names, rep lists and children */
    img_cache_shutdown();
    g_last_fb = NULL;
}

/* ------------------------------------------------------------ frame + render */

void gfx_frame(int32_t game_dt, int32_t real_dt)
{
    img_cache_trim();         /* the frame boundary: images of the last frame may be evicted now */
    for (int s = 0; s < GFX_STAGE_CNT; s++)
        for (int i = 0; i < GFX_OBJ_CNT; i++) obj_frame(gfx_obj(s, i), game_dt, real_dt);
    for (int i = 0; i < GFX_EFFECT_CNT; i++) effect_frame(&g_effect[i], game_dt, real_dt);
    effect_frame(&g_screen, game_dt, real_dt);
    gfx_wipe_frame(game_dt);
    shake_frame(game_dt);
}

void gfx_render(Canvas *fb)
{
    if (!fb || !fb->px) return;
    g_last_fb = fb;
    int sdx, sdy;
    gfx_shake_offset(&sdx, &sdy);
    Canvas *t = (sdx || sdy) ? &g_scene : fb;
    canvas_clear(fb, 0xff000000u);
    if (t != fb) canvas_clear(t, 0xff000000u);
    gfx_wipe_draw(t);
    for (int i = 0; i < GFX_EFFECT_CNT; i++) effect_apply(t, &g_effect[i]);
    effect_apply(t, &g_screen);
    if (t != fb) canvas_copy(fb, t, sdx, sdy);   /* shake: uncovered area stays black [I] */
}

const Canvas *gfx_last_fb(void) { return g_last_fb; }
void gfx_forget_fb(const Canvas *fb) { if (g_last_fb == fb) g_last_fb = NULL; }

/* ------------------------------------------------------------ save thumbnail */

static int scale_div(int a, int b, int c) { return (int)((int64_t)a * b / c); }   /* a*b/c, c > 0 */

void gfx_capture(Canvas *out)
{
    if (!out || !out->px || out->w <= 0 || out->h <= 0) return;
    canvas_clear(out, 0xff000000u);
    const Canvas *src = g_last_fb;
    if (!src || !src->px || src->w <= 0 || src->h <= 0) return;
    /* letterboxed box filter (keeps the aspect ratio) */
    int dw = out->w, dh = scale_div(src->h, out->w, src->w);
    if (dh > out->h) { dh = out->h; dw = scale_div(src->w, out->h, src->h); }
    int ox = (out->w - dw) / 2, oy = (out->h - dh) / 2;
    Canvas box = { out->px + (size_t)oy * (size_t)out->pitch + (size_t)ox, dw, dh, out->pitch };
    canvas_downscale_box(&box, src);
}
