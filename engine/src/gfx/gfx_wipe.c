/* gfx/gfx_wipe.c — wipe state (DAT_00a8fefc), back->front transfer, progress and drawing of the running wipe
 * (GFX §2.1, §4.1–4.2). The three working canvases (OLD, NEW, the #LOAD.WIPE snapshot) are static screen-sized
 * buffers: no allocation, nothing to free, always valid. */
#include "gfx/gfx.h"
#include "core/log.h"
#include "gfx/render.h"
#include "vm/proc.h"
#include <limits.h>
#include <string.h>

enum { WIPE_NONE = -1, WIPE_OPT_CNT = 8, WIPE_P_ONE = 65536 };

typedef struct WipeState {
    int type;               /* WIPE_NONE = no wipe */
    int speed, opt[WIPE_OPT_CNT];
    int elapsed, start_time, time;
    WipeRange range;
    int phase, progress;
    bool load_wipe;         /* OLD = captured framebuffer (#LOAD.WIPE), no transfer */
} WipeState;

static WipeState g_wipe = { .type = WIPE_NONE };
static uint32_t g_old_px[SCREEN_W * SCREEN_H], g_new_px[SCREEN_W * SCREEN_H], g_snap_px[SCREEN_W * SCREEN_H];
static Canvas g_old = { g_old_px, SCREEN_W, SCREEN_H, SCREEN_W };
static Canvas g_new = { g_new_px, SCREEN_W, SCREEN_H, SCREEN_W };
static Canvas g_snap = { g_snap_px, SCREEN_W, SCREEN_H, SCREEN_W };

static void wipe_end(void)   /* FUN_005581d0 -> FUN_005c9c00(0) + FUN_00557d90 */
{
    if (g_wipe.type == WIPE_NONE) return;
    if (log_trace_on("GFX")) log_trace("GFX", "wipe end type=%d elapsed=%d", g_wipe.type, g_wipe.elapsed);
    g_wipe.type = WIPE_NONE;
    g_wipe.load_wipe = false;
    for (int i = 0; i < GFX_OBJ_CNT; i++) obj_init(gfx_obj(GFX_STAGE_NEXT, i));   /* next cleared [I] */
}

void gfx_wipe_reset(void)
{
    g_wipe.type = WIPE_NONE;
    g_wipe.load_wipe = false;
}

void wipe_args_default(WipeArgs *a)   /* FUN_0066d380 defaults (GFX §4.1) */
{
    memset(a, 0, sizeof *a);
    a->type = 0; a->time = 500; a->speed = 0;
    a->begin_order = 0; a->end_order = 0; a->begin_layer = INT_MIN; a->end_layer = INT_MAX;
    a->wait = true; a->key = -1; a->start_time = 0;
}

/* FUN_005ca440: back -> front transfer at wipe start (GFX §2.1, CONFIRMED asm 0x5ca480..0x5ca5b3) */
static void wipe_transfer(const WipeRange *r)
{
    for (int i = 0; i < GFX_OBJ_CNT; i++) {
        Obj *F = gfx_obj(GFX_STAGE_FRONT, i), *B = gfx_obj(GFX_STAGE_BACK, i), *N = gfx_obj(GFX_STAGE_NEXT, i);
        bool in_range = render_range_contains(r, F->order, F->layer);
        bool b_has = B->type != OBJ_NONE || B->nchild > 0;
        if (!in_range && !b_has) continue;
        obj_copy(N, F);
        if (!F->wipe_copy || b_has || B->wipe_erase) {
            obj_copy(F, B);
            obj_init(B);
        }
    }
}

/* #CONFIG wipe switches: the game's Gameexe values until ui_config_apply pushes the user's. */
static bool g_skip_wipe_anime = true, g_no_wipe_anime;   /* config ff08+0x18a / fee8+0x189 */

void gfx_set_wipe_options(bool skip_wipe_anime, bool no_wipe_anime)
{
    g_skip_wipe_anime = skip_wipe_anime;
    g_no_wipe_anime = no_wipe_anime;
}

static int key_mode(int key)
{
    if (key == 0 || key == 1) return key;
    return g_skip_wipe_anime;
}

void gfx_push_wipe_wait(int key, bool push_result)
{
    proc_push_wait(PROC_WIPE_WAIT, 0, key_mode(key) != 0, push_result);
}

static void wipe_begin(const WipeArgs *a, bool transfer, bool load_wipe)
{
    wipe_end();   /* FUN_00557eb0 ends a running wipe first */
    g_wipe.type = a->type < 0 ? WIPE_NONE : a->type;   /* a negative type is no wipe (as type < 0 always was) */
    g_wipe.speed = a->speed;
    memcpy(g_wipe.opt, a->opt, sizeof g_wipe.opt);
    g_wipe.time = a->time;
    g_wipe.start_time = a->start_time;
    g_wipe.elapsed = 0;
    g_wipe.range = (WipeRange){ a->begin_order, a->begin_layer, a->end_order, a->end_layer };
    g_wipe.phase = 0;
    g_wipe.progress = 0;
    g_wipe.load_wipe = load_wipe;
    if (transfer) wipe_transfer(&g_wipe.range);
    if (log_trace_on("GFX"))
        log_trace("GFX", "wipe start type=%d time=%d opts=[%d,%d,%d,%d] wait=%d", a->type, a->time,
                  a->opt[0], a->opt[1], a->opt[2], a->opt[3], (int)a->wait);
    if (a->wait) gfx_push_wipe_wait(a->key, false);   /* FUN_00558220(n9, 0) */
    if (g_no_wipe_anime) wipe_end();
}

void gfx_wipe_start(const WipeArgs *a) { wipe_begin(a, true, false); }
bool gfx_wipe_active(void) { return g_wipe.type != WIPE_NONE; }
void gfx_wipe_finish(void) { wipe_end(); }
int  gfx_wipe_progress(void) { return g_wipe.progress; }

/* double -> int, out of range (or NaN) -> INT_MIN like the x86 cvttsd2si of the original. */
static int to_int(double v) { return (v > -2147483649.0 && v < 2147483648.0) ? (int)v : INT_MIN; }

int wipe_calc_progress(int elapsed, int time, int speed)   /* FUN_00558320 */
{
    double e = elapsed, t = time;
    if (time < 1) return WIPE_P_ONE;
    switch (speed) {
    case 0: return to_int(e * 65536.0 / t);
    case 1: return to_int(e * e * 65536.0 / t / t);
    case 2: return (int)((uint32_t)WIPE_P_ONE - (uint32_t)to_int((e - t) * (e - t) * 65536.0 / t / t));
    default: return 0;
    }
}

void gfx_wipe_frame(int32_t game_dt)
{
    if (g_wipe.type == WIPE_NONE) return;
    if (g_wipe.phase == 0) { g_wipe.phase = 1; return; }      /* one frame is skipped */
    if (g_wipe.phase == 1) { g_wipe.elapsed = g_wipe.start_time; g_wipe.phase = 2; }
    else g_wipe.elapsed = core_wrap_add_i32(g_wipe.elapsed, game_dt);
    g_wipe.progress = wipe_calc_progress(g_wipe.elapsed, g_wipe.time, g_wipe.speed);
    if ((int64_t)g_wipe.elapsed - g_wipe.time >= 0) wipe_end();
}

void gfx_start_load_wipe(int type, int ms)
{
    const Canvas *fb = gfx_last_fb();
    if (!fb || !fb->px) return;
    canvas_clear(&g_snap, 0);
    canvas_copy(&g_snap, fb, 0, 0);
    WipeArgs a;
    wipe_args_default(&a);
    a.type = type; a.time = ms; a.wait = false;
    a.begin_order = INT_MIN; a.end_order = INT_MAX;
    wipe_begin(&a, false, true);
}

void gfx_wipe_draw(Canvas *t)
{
    if (g_wipe.type == WIPE_NONE) {
        render_stage(t, GFX_STAGE_FRONT, NULL);
    } else if (g_wipe.load_wipe) {
        canvas_clear(&g_new, 0);
        render_stage(&g_new, GFX_STAGE_FRONT, NULL);
        wipe_composite(t, &g_snap, &g_new, g_wipe.type, g_wipe.progress, g_wipe.opt, WIPE_OPT_CNT);
    } else {
        /* FUN_00561820 draws below `begin`, the wipe renderer the range, FUN_00561ae0 above `end` */
        render_stage_range(t, GFX_STAGE_FRONT, NULL, &g_wipe.range, RANGE_BELOW);
        canvas_clear(&g_old, 0);
        canvas_clear(&g_new, 0);
        render_stage_range(&g_old, GFX_STAGE_NEXT, NULL, &g_wipe.range, RANGE_IN);
        render_stage_range(&g_new, GFX_STAGE_FRONT, NULL, &g_wipe.range, RANGE_IN);
        wipe_composite(t, &g_old, &g_new, g_wipe.type, g_wipe.progress, g_wipe.opt, WIPE_OPT_CNT);
        render_stage_range(t, GFX_STAGE_FRONT, NULL, &g_wipe.range, RANGE_ABOVE);
    }
}
