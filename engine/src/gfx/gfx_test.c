/* gfx/gfx_test.c — window-less unit tests of the gfx module (ENGINE_SPEC §5 WP3 self-checks): IntEvent curves,
 * wipe compositing and progress, sort order, the back->front transfer, rendering, shake, screen effects, and the
 * hardening limits (extreme coordinates, wrapped event time, object depth cap). */
#include "gfx/gfx.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "gfx/render.h"
#include <limits.h>
#include <stdlib.h>

static int st_fail;
#define ST_CHECK(cond, ...) do { if (!(cond)) { st_fail++; log_msg(LOG_ERR, "gfx_selftest: " __VA_ARGS__); } } while (0)

static int ev_at(IntEvent e, int ms)   /* evaluate an event after `ms` of game time in 10 ms frames */
{
    for (int t = 0; t < ms; t += 10) ievent_frame(&e, 10, 10);
    return e.cur;
}

static void st_events(void)
{
    IntEvent e;
    ievent_init(&e, 0);
    ievent_set(&e, 100, 1000, 0, 0, false, 0, false);
    ST_CHECK(e.value == 100, "set: value must be the target right away");
    ST_CHECK(ev_at(e, 500) == 50, "linear t=500 -> %d", ev_at(e, 500));
    ST_CHECK(ev_at(e, 250) == 25, "linear t=250 -> %d", ev_at(e, 250));
    IntEvent d = e;
    for (int t = 0; t < 1000; t += 10) ievent_frame(&d, 10, 10);
    ST_CHECK(d.loop == -1 && d.cur == 100, "one-shot ends at total (loop=%d cur=%d)", d.loop, d.cur);
    ievent_init(&e, 0); ievent_set(&e, 100, 1000, 0, 1, false, 0, false);
    ST_CHECK(ev_at(e, 500) == 25, "ease-in t=500 -> %d", ev_at(e, 500));
    ievent_init(&e, 0); ievent_set(&e, 100, 1000, 0, 2, false, 0, false);
    ST_CHECK(ev_at(e, 500) == 75, "ease-out t=500 -> %d", ev_at(e, 500));
    ievent_init(&e, 0); ievent_set(&e, 100, 1000, 200, 0, false, 0, false);
    ST_CHECK(ev_at(e, 150) == 0, "delay: cur stays at start");
    ST_CHECK(ev_at(e, 700) == 50, "delay 200 t=700 -> %d", ev_at(e, 700));
    ievent_init(&e, 7); ievent_set(&e, 0, 800, 0, 0, true, 255, false);   /* tr_eve.set(0,800,0,0,start=255) */
    ST_CHECK(e.start == 255 && e.value == 0 && ev_at(e, 400) == 127, "named start -> %d", ev_at(e, 400));
    ievent_init(&e, 5);
    ievent_loop(&e, 0, 100, 1000, 0, 0, 1, false);
    ST_CHECK(ev_at(e, 1500) == 50 && e.value == 5, "loop t=1500 -> %d", ev_at(e, 1500));
    ievent_loop(&e, 0, 100, 1000, 0, 0, 2, false);
    ST_CHECK(ev_at(e, 1200) == 80, "turn t=1200 -> %d", ev_at(e, 1200));
    ST_CHECK(ev_at(e, 500) == 50, "turn t=500 -> %d", ev_at(e, 500));
    IntEvent r = e; r.real = 1;
    ievent_frame(&r, 0, 300);
    ST_CHECK(r.time == 300, "real clock flag uses real_dt");
    ievent_end(&e); ievent_frame(&e, 10, 10);
    ST_CHECK(e.cur == 5 && e.loop == -1, "end snaps back to value");
}

/* Script extremes: time wraps, huge totals and delays stay defined. */
static void st_event_limits(void)
{
    IntEvent e;
    ievent_init(&e, 0);
    ievent_loop(&e, INT_MIN, INT_MAX, INT_MAX, INT_MIN, 1, 2, false);
    for (int k = 0; k < 4; k++) ievent_frame(&e, INT_MAX, INT_MAX);
    ST_CHECK(e.loop == 2, "turn with extreme values keeps running");
    ievent_set(&e, INT_MAX, 1, INT_MAX, 0, true, INT_MIN, false);
    ievent_frame(&e, INT_MIN, 0);
    ST_CHECK(e.loop == 0 && e.cur == INT_MIN, "negative wrapped time stays at start (cur=%d)", e.cur);
}

static void st_fill(Canvas *c, uint32_t v) { for (int i = 0; i < c->w * c->h; i++) c->px[i] = v; }

static void st_wipe_basic(Canvas *o, Canvas *n, Canvas *d)
{
    /* M1.5: type 0 at p = 32768 equals (OLD+NEW)/2 +-1 */
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 0, 32768, NULL, 0);
    uint32_t p = d->px[1234];
    int rr = (int)((p >> 16) & 255), gg = (int)((p >> 8) & 255), bb = (int)(p & 255);
    ST_CHECK(abs(rr - 0x80) <= 1 && abs(gg - 0x80) <= 1 && abs(bb - 0x80) <= 1, "type0 half: %08x", (unsigned)p);
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 1, 0, NULL, 0);
    ST_CHECK(d->px[5000] == 0xffe0c0a0u, "type1 must draw NEW");
    /* M1.5: type 100 d=0 at P=0.5: rows < 240 NEW, rows >= 240 OLD (+-1 row) */
    int opts[8] = { 0, 0 };
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 100, 32768, opts, 8);
    int edge = -1;
    for (int y = 0; y < SCREEN_H; y++) if (d->px[(size_t)y * d->pitch + 320] == 0xff204060u) { edge = y; break; }
    ST_CHECK(edge >= 239 && edge <= 241, "type100 d0 edge row %d", edge);
    ST_CHECK(d->px[10] == 0xffe0c0a0u && d->px[(size_t)470 * d->pitch] == 0xff204060u,
             "type100 d0 top NEW / bottom OLD");
    opts[1] = 1;   /* d=1: bottom first */
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 100, 16384, opts, 8);
    ST_CHECK(d->px[(size_t)470 * d->pitch] == 0xffe0c0a0u && d->px[10] == 0xff204060u, "type100 d1 bottom first");
    ST_CHECK(wipe_mask_alpha256(255, 0, 0) == 0 && wipe_mask_alpha256(0, 65536, 0) == 256, "mask alpha ends");
}

static void st_wipe_masks(void)
{
    /* 101 {0,d,16}: 16-px bands, first band opens first; 102 {0,0,16}: gradient repeats every 16 px */
    int o101[8] = { 0, 0, 16 }, o102[8] = { 0, 0, 16 };
    ST_CHECK(wipe_mask_value(101, o101, 8, 0, 0, SCREEN_W, SCREEN_H) == 255, "101 m(0)");
    ST_CHECK(wipe_mask_value(101, o101, 8, 0, 479, SCREEN_W, SCREEN_H) == 0, "101 m(479)");
    ST_CHECK(wipe_mask_value(101, o101, 8, 0, 15, SCREEN_W, SCREEN_H) == 127, "101 m(15)=%d",
             wipe_mask_value(101, o101, 8, 0, 15, SCREEN_W, SCREEN_H));
    ST_CHECK(wipe_mask_value(102, o102, 8, 0, 3, SCREEN_W, SCREEN_H) ==
                     wipe_mask_value(102, o102, 8, 0, 19, SCREEN_W, SCREEN_H) &&
                 wipe_mask_value(102, o102, 8, 0, 0, SCREEN_W, SCREEN_H) == 255 &&
                 wipe_mask_value(102, o102, 8, 0, 15, SCREEN_W, SCREEN_H) == 0,
             "102 wraps every 16 px");
    o102[1] = 2;
    ST_CHECK(wipe_mask_value(102, o102, 8, 16, 0, SCREEN_W, SCREEN_H) == 255 &&
             wipe_mask_value(102, o102, 8, 31, 0, SCREEN_W, SCREEN_H) == 0, "102 d2 vertical blinds");
    /* progress curves (FUN_00558320) */
    ST_CHECK(wipe_calc_progress(250, 1000, 0) == 16384 && wipe_calc_progress(500, 1000, 1) == 16384 &&
             wipe_calc_progress(500, 1000, 2) == 49152 && wipe_calc_progress(0, 0, 0) == 65536, "progress curves");
}

static void st_wipe_moves(Canvas *o, Canvas *n, Canvas *d)
{
    /* 200 {0,1,1} at P=0.5: NEW slid down by half (row 0 = NEW row 240), OLD below */
    for (int y = 0; y < SCREEN_H; y++)
        for (int x = 0; x < SCREEN_W; x++) n->px[(size_t)y * n->pitch + x] = 0xff000000u | (uint32_t)(y / 2);
    int o200[8] = { 0, 1, 1 };
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 200, 32768, o200, 8);
    ST_CHECK(d->px[0] == (0xff000000u | 120u) && d->px[(size_t)300 * d->pitch] == 0xff204060u, "200 push: %08x %08x",
             (unsigned)d->px[0], (unsigned)d->px[(size_t)300 * d->pitch]);
    /* 211 {5}: OLD squashed to the centre line: at P=0.5 rows 0..119 show NEW */
    st_fill(n, 0xffe0c0a0u);
    int o211[8] = { 5 };
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 211, 32768, o211, 8);
    ST_CHECK(d->px[(size_t)60 * d->pitch] == 0xffe0c0a0u && d->px[(size_t)240 * d->pitch] == 0xff204060u, "211 squash");
    /* 214: OLD fades out: at P=1 only NEW */
    canvas_clear(d, 0xff000000u);
    wipe_composite(d, o, n, 214, 65536, NULL, 0);
    ST_CHECK(d->px[777] == 0xffe0c0a0u, "214 end shows NEW");
}

/* Hostile geometry draws nothing and touches nothing out of range. */
static void st_canvas_limits(Canvas *o, Canvas *d)
{
    canvas_clear(d, 0xff000000u);
    Image im = { o->px, o->w, o->h, 0, 0 };
    canvas_blit(d, &im, rect_make(INT_MIN, INT_MIN, INT_MAX, INT_MAX), INT_MAX, INT_MAX, 255, NULL);
    canvas_blit(d, &im, rect_make(0, 0, o->w, o->h), INT_MIN, INT_MIN, 255, NULL);
    canvas_blit_ex(d, &im, rect_make(0, 0, o->w, o->h), 1e30f, -1e30f, 1e-30f, 1e30f, 255, NULL, NULL);
    canvas_fill(d, rect_make(INT_MIN, INT_MIN, INT_MAX, INT_MIN + 1), 0xffffffffu, 255);
    ST_CHECK(d->px[0] == 0xff000000u && d->px[(size_t)SCREEN_H * SCREEN_W - 1] == 0xff000000u,
             "extreme blits draw nothing");
    Rect r = rect_at(INT_MAX, INT_MIN, INT_MAX, INT_MAX);
    ST_CHECK(r.x0 <= r.x1 && r.y0 <= r.y1, "rect_at does not wrap");
    Canvas bad;
    ST_CHECK(!canvas_alloc(&bad, -1, 10) && !canvas_alloc(&bad, 1 << 20, 1 << 20) && !bad.px,
             "canvas_alloc refuses bad sizes");
}

static void st_wipes(void)
{
    Canvas o, n, d;
    bool ok = canvas_alloc(&o, SCREEN_W, SCREEN_H) && canvas_alloc(&n, SCREEN_W, SCREEN_H) &&
              canvas_alloc(&d, SCREEN_W, SCREEN_H);
    ST_CHECK(ok, "canvas_alloc");
    if (ok) {
        st_fill(&o, 0xff204060u); st_fill(&n, 0xffe0c0a0u);
        st_wipe_basic(&o, &n, &d);
        st_wipe_moves(&o, &n, &d);
        st_canvas_limits(&o, &d);
    }
    st_wipe_masks();
    canvas_free(&o); canvas_free(&n); canvas_free(&d);
}

static void st_sort(void)
{
    gfx_reset_all();
    Obj *f = gfx_obj(GFX_STAGE_FRONT, 0);
    obj_create_rect(&f[3], 0, 0, 10, 10, 255, 0, 0, 255); f[3].layer = 0;
    obj_create_rect(&f[1], 0, 0, 10, 10, 0, 255, 0, 255); f[1].layer = 5;
    obj_create_rect(&f[2], 0, 0, 10, 10, 0, 0, 255, 255); f[2].layer = 0;
    obj_create_rect(&f[99], 0, 0, 10, 10, 0, 0, 0, 255); f[99].layer = -9999;
    obj_create_rect(&f[7], 0, 0, 10, 10, 0, 0, 0, 255); f[7].order = 1; f[7].layer = -100;
    int idx[GFX_OBJ_CNT];
    int m = render_sorted(f, GFX_OBJ_CNT, idx);
    ST_CHECK(m == 5 && idx[0] == 99 && idx[1] == 2 && idx[2] == 3 && idx[3] == 1 && idx[4] == 7,
             "sort order (order, layer, index): %d %d %d %d %d", idx[0], idx[1], idx[2], idx[3], idx[4]);
}

/* $bg_disp transfer (GFX §2.1): back emptied, obj99 survives with wipe_copy, chr cleared. */
static void st_transfer(void)
{
    gfx_reset_all();
    WStr nm = { 0 };
    ws_set_utf8(&nm, "__selftest_missing__");
    Obj *b = gfx_obj(GFX_STAGE_BACK, 0), *f = gfx_obj(GFX_STAGE_FRONT, 0), *nx = gfx_obj(GFX_STAGE_NEXT, 0);
    obj_create_image(&f[2], &nm); f[2].disp = true;             /* a character on front */
    obj_create_rect(&b[99], 0, 0, 640, 480, 0, 0, 0, 255);      /* $bg_disp body */
    b[99].disp = true; b[99].wipe_copy = true; b[99].layer = -9999;
    obj_create_rect(&b[0], 0, 0, 640, 480, 10, 20, 30, 255); b[0].disp = true;
    WipeArgs a;
    wipe_args_default(&a);
    a.type = 0; a.time = 100; a.wait = false;
    gfx_wipe_start(&a);
    ST_CHECK(b[0].type == OBJ_NONE && b[99].type == OBJ_NONE, "back emptied after transfer");
    ST_CHECK(f[0].type == OBJ_RECT && f[99].type == OBJ_RECT && f[99].wipe_copy, "front got the new picture");
    ST_CHECK(f[2].type == OBJ_NONE && nx[2].type == OBJ_IMAGE, "in-range front object cleared, old kept in next");
    ST_CHECK(gfx_wipe_active(), "wipe active");
    gfx_frame(16, 16); ST_CHECK(gfx_wipe_progress() == 0, "phase 0 frame");
    gfx_frame(16, 16); ST_CHECK(gfx_wipe_progress() == 0, "phase 1 frame (elapsed = start_time)");
    gfx_frame(50, 50); ST_CHECK(gfx_wipe_progress() == 32768, "progress after 50/100 ms: %d", gfx_wipe_progress());
    gfx_frame(50, 50); ST_CHECK(!gfx_wipe_active() && nx[2].type == OBJ_NONE, "wipe ended, next cleared");
    /* second $bg_disp without obj99 on back: obj99 survives (wipe_copy) */
    obj_create_rect(&b[0], 0, 0, 640, 480, 1, 2, 3, 255);
    a.time = 0;
    gfx_wipe_start(&a);
    ST_CHECK(f[99].type == OBJ_RECT && f[0].rect_rgb == 0xff010203u, "wipe_copy object survives the next wipe");
    gfx_wipe_finish();
    ws_free(&nm);
}

/* Front stage of st_render: blue backdrop, a half-transparent red rect, a clipped parent with a green child. */
static Obj *st_render_scene(void)
{
    gfx_reset_all();
    Obj *f = gfx_obj(GFX_STAGE_FRONT, 0);
    obj_create_rect(&f[0], 0, 0, 640, 480, 0, 0, 255, 255); f[0].disp = true;
    obj_create_rect(&f[1], 0, 0, 100, 100, 255, 0, 0, 255); f[1].disp = true;
    ievent_assign(&f[1].x, 50); ievent_assign(&f[1].y, 60); ievent_assign(&f[1].tr, 128);
    f[5].disp = true; ievent_assign(&f[5].x, 300);
    ST_CHECK(obj_children_resize(&f[5], 2), "resize");
    obj_create_rect(&f[5].child[1], 0, 0, 20, 20, 0, 255, 0, 255); f[5].child[1].disp = true;
    ievent_assign(&f[5].child[1].y, 10);
    f[5].clip_use = true; ievent_assign(&f[5].clip_r, 310); ievent_assign(&f[5].clip_b, 480);
    return f;
}

static void st_render(Canvas *fb)
{
    Obj *f = st_render_scene();
    gfx_frame(0, 0);
    gfx_render(fb);
    uint32_t c0 = fb->px[0], c1 = fb->px[(size_t)70 * fb->pitch + 60];
    uint32_t c2 = fb->px[(size_t)15 * fb->pitch + 305], c3 = fb->px[(size_t)15 * fb->pitch + 315];
    ST_CHECK(c0 == 0xff0000ffu, "rect bg %08x", (unsigned)c0);
    ST_CHECK(((c1 >> 16) & 255) >= 127 && ((c1 >> 16) & 255) <= 129 && (c1 & 255) >= 126 && (c1 & 255) <= 128,
             "tr 128 blend %08x", (unsigned)c1);
    ST_CHECK(c2 == 0xff00ff00u && c3 == 0xff0000ffu, "child pos + parent clip %08x %08x", (unsigned)c2, (unsigned)c3);
    Rect hr;
    ST_CHECK(obj_screen_rect(&f[5].child[1], &hr) && hr.x0 == 300 && hr.y0 == 10 && hr.x1 == 310 && hr.y1 == 30,
             "child hit rect");
    ST_CHECK(gfx_obj_hit(&f[1], 60, 70) && !gfx_obj_hit(&f[1], 10, 10), "hit test");
    /* copy is deep */
    obj_copy(&f[6], &f[5]);
    ST_CHECK(f[6].nchild == 2 && f[6].child != f[5].child && f[6].child[1].parent == &f[6], "deep copy");
    int ch[] = { 38, 2, -1, 6, 93, -1, 1 };
    ST_CHECK(gfx_resolve_obj(ch, 7) == &f[6].child[1] && gfx_resolve_obj(ch, 4) == &f[6], "resolve object chain");
}

static void st_shake_and_effects(Canvas *fb)
{
    /* shake 2: (6,32,32)(0,0,32)(-4,32,32) */
    int dx, dy;
    ST_CHECK(gfx_shake_start(2), "shake start");
    gfx_shake_offset(&dx, &dy); ST_CHECK(dx == 6 && dy == 32, "shake step 0");
    gfx_frame(40, 40); gfx_shake_offset(&dx, &dy); ST_CHECK(dx == 0 && dy == 0, "shake step 1");
    gfx_frame(30, 30); gfx_shake_offset(&dx, &dy); ST_CHECK(dx == -4 && dy == 32, "shake step 2");
    gfx_render(fb);
    ST_CHECK(fb->px[(size_t)10 * fb->pitch + 320] == 0xff000000u &&
                 fb->px[(size_t)100 * fb->pitch + 320] == 0xff0000ffu,
             "shake translates the picture");
    gfx_frame(40, 40); ST_CHECK(!gfx_shake_active(), "shake ends");
    /* screen effect: mono + bright = white, then mono grey */
    Effect *e = gfx_effect(0);
    ievent_assign(&e->tone[TONE_MONO], 255); ievent_assign(&e->tone[TONE_BRIGHT], 255);
    gfx_render(fb);
    ST_CHECK(fb->px[0] == 0xffffffffu, "effect bright flash %08x", (unsigned)fb->px[0]);
    ievent_assign(&e->tone[TONE_BRIGHT], 0);
    gfx_render(fb);
    uint32_t g = fb->px[0];
    ST_CHECK(((g >> 16) & 255) == (g & 255) && ((g >> 8) & 255) == (g & 255), "effect mono grey %08x", (unsigned)g);
    gfx_effect_init(e);
}

/* Child arrays and rep lists past the MEM_OBJ budget are refused and leave the object as it was (review finding:
 * a save or script could ask for gigabytes of objects). */
static void st_obj_budget(void)
{
    gfx_reset_all();
    size_t keep = mem_budget(MEM_OBJ);
    mem_budget_set(MEM_OBJ, 2u << 20);                 /* try allocations may use 1 MiB of it */
    Obj *o = gfx_obj(GFX_STAGE_FRONT, 11);
    ST_CHECK(!obj_children_resize(o, GFX_OBJ_CHILD_MAX) && o->nchild == 0, "child array past the budget refused");
    ST_CHECK(obj_children_resize(o, 8) && o->nchild == 8, "small child array accepted");
    ST_CHECK(!obj_rep_resize(&o->x_rep, &o->nx_rep, GFX_REP_MAX * 8) && o->nx_rep == 0, "rep list out of range");
    ST_CHECK(obj_rep_resize(&o->x_rep, &o->nx_rep, 16) && o->nx_rep == 16, "small rep list accepted");
    obj_copy(gfx_obj(GFX_STAGE_FRONT, 12), o);
    ST_CHECK(gfx_obj(GFX_STAGE_FRONT, 12)->nchild == 8, "copy within the budget");
    mem_budget_set(MEM_OBJ, keep);
    gfx_reset_all();
}

/* Children stop at GFX_OBJ_DEPTH_MAX; copying a parent into its own deepest child stays within the cap. */
static void st_depth_cap(void)
{
    gfx_reset_all();
    Obj *o = gfx_obj(GFX_STAGE_FRONT, 10);
    for (int d = 0; d < GFX_OBJ_DEPTH_MAX + 3 && o; d++) {
        (void)obj_children_resize(o, 1);   /* refused at the depth cap */
        o = o->nchild ? &o->child[0] : NULL;
    }
    int depth = 0;
    for (const Obj *p = gfx_obj(GFX_STAGE_FRONT, 10); p->nchild; p = &p->child[0]) depth++;
    ST_CHECK(depth == GFX_OBJ_DEPTH_MAX, "child depth capped at %d (got %d)", GFX_OBJ_DEPTH_MAX, depth);
    Obj *leaf = gfx_obj(GFX_STAGE_FRONT, 10);
    while (leaf->nchild) leaf = &leaf->child[0];
    obj_copy(leaf, gfx_obj(GFX_STAGE_FRONT, 10));
    ST_CHECK(obj_depth(leaf) == GFX_OBJ_DEPTH_MAX && leaf->nchild == 0, "copy into the deepest child is pruned");
    gfx_frame(0, 0);
    gfx_reset_all();
}

static void st_objects(void)
{
    st_sort();
    st_transfer();
    Canvas fb;
    if (!canvas_alloc(&fb, SCREEN_W, SCREEN_H)) { ST_CHECK(false, "canvas_alloc"); return; }
    st_render(&fb);
    st_shake_and_effects(&fb);
    gfx_forget_fb(&fb);   /* fb is a local: no dangling pointer for later load wipes / captures */
    canvas_free(&fb);
    st_depth_cap();
    st_obj_budget();
    gfx_reset_all();
}

int gfx_selftest(void)
{
    st_fail = 0;
    gfx_init();
    st_events();
    st_event_limits();
    st_wipes();
    st_objects();
    log_msg(st_fail ? LOG_ERR : LOG_INFO, "gfx_selftest: %d failure(s)", st_fail);
    return st_fail;
}
