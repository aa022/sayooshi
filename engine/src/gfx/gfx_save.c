/* gfx/gfx_save.c — objects and screen effects in save files (ENGINE_SPEC §2.11, docs/SAVE_FORMAT.md §9.2).
 * GFX1 (written): only objects of stages 0/1 with #OBJECT.000-099.SAVE=1 that differ from a freshly initialised
 * object, and inside an object only the fields that differ (presence bit masks). GFX0 (format-1 files): every
 * saved object in full; still read. Every count is bounded (sav_get_count) and object trees are read at most
 * GFX_OBJ_DEPTH_MAX deep; a damaged chunk leaves an empty screen. */
#include "gfx/gfx.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/gameexe.h"
#include "gfx/render.h"
#include "sav/sav_stream.h"
#include <stddef.h>
#include <string.h>

enum { GFX_SAVE_VERSION = 1, EV_FIELDS = 11, EFFECT_EVENTS = 3 + TONE_CNT, EFFECT_SCALARS = 6, REP_LISTS = 4 };

/* ---- IntEvent fields in disk order: def, value, time, total, delay, start, cur, end, loop, speed, real ---- */

static void ev_fields(const IntEvent *e, int32_t v[EV_FIELDS])
{
    v[0] = e->def; v[1] = e->value; v[2] = e->time; v[3] = e->total; v[4] = e->delay; v[5] = e->start;
    v[6] = e->cur; v[7] = e->end; v[8] = e->loop; v[9] = e->speed; v[10] = e->real;
}

static void ev_set_fields(IntEvent *e, const int32_t v[EV_FIELDS])
{
    e->def = v[0]; e->value = v[1]; e->time = v[2]; e->total = v[3]; e->delay = v[4]; e->start = v[5];
    e->cur = v[6]; e->end = v[7]; e->loop = v[8]; e->speed = v[9]; e->real = v[10];
}

static void w_ev(SavWriter *w, const IntEvent *e)
{
    int32_t v[EV_FIELDS];
    ev_fields(e, v);
    for (int k = 0; k < EV_FIELDS; k++) sav_put_i32(w, v[k]);
}

static void r_ev(SavReader *r, IntEvent *e)
{
    int32_t v[EV_FIELDS];
    for (int k = 0; k < EV_FIELDS; k++) v[k] = sav_get_i32(r);
    ev_set_fields(e, v);
}

static void r_evlist(SavReader *r, IntEvent **l, int *n)
{
    int c = sav_get_count(r, GFX_REP_MAX, (size_t)EV_FIELDS * 4);
    if (sav_ok(r) && !obj_rep_resize(l, n, c)) sav_fail(r);
    for (int k = 0; k < c && k < *n; k++) r_ev(r, &(*l)[k]);
}

static bool ev_same(const IntEvent *a, const IntEvent *b)
{
    int32_t x[EV_FIELDS], y[EV_FIELDS];
    ev_fields(a, x);
    ev_fields(b, y);
    return !memcmp(x, y, sizeof x);
}

/* Event = u32 field mask (bit k = field k) + the values of the set bits. */
static void w_ev_diff(SavWriter *w, const IntEvent *e, const IntEvent *base)
{
    int32_t v[EV_FIELDS], b[EV_FIELDS];
    ev_fields(e, v);
    ev_fields(base, b);
    uint32_t m = 0;
    for (int k = 0; k < EV_FIELDS; k++) if (v[k] != b[k]) m |= 1u << k;
    sav_put_u32(w, m);
    for (int k = 0; k < EV_FIELDS; k++) if (m >> k & 1) sav_put_i32(w, v[k]);
}

static void r_ev_diff(SavReader *r, IntEvent *e)   /* e holds the default already */
{
    int32_t v[EV_FIELDS];
    ev_fields(e, v);
    uint32_t m = sav_get_u32(r);
    for (int k = 0; k < EV_FIELDS; k++) if (m >> k & 1) v[k] = sav_get_i32(r);
    for (int k = EV_FIELDS; k < 32; k++) if (m >> k & 1) (void)sav_get_i32(r);   /* future fields */
    ev_set_fields(e, v);
}

static void r_ev_skip(SavReader *r) { IntEvent scratch; ievent_init(&scratch, 0); r_ev_diff(r, &scratch); }

static void set_img_key(Obj *o)
{
    if (o->type == OBJ_IMAGE) { obj_set_image(o, &o->file); return; }
    mem_free(o->img_key);
    o->img_key = NULL;
}

/* ---- format 1 reader (GFX0) ---- */

static void read_misc_v1(SavReader *r, Obj *o)
{
    int *misc[] = { &o->world, &o->blend, &o->tonecurve, &o->click_disable, &o->light_no, &o->fog_use, &o->mask_no,
                    &o->culling, &o->alpha_test, &o->alpha_blend };
    for (size_t k = 0; k < COUNT_OF(misc); k++) *misc[k] = sav_get_i32(r);
    o->is_button = sav_get_u8(r);
    o->btn_no = sav_get_i32(r); o->btn_group = sav_get_i32(r); o->btn_action = sav_get_i32(r);
    o->btn_se = sav_get_i32(r); o->btn_state = sav_get_i32(r);
}

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void read_obj_v1(SavReader *r, Obj *o, int depth)
{
    obj_init(o);
    o->type = sav_get_i32(r);
    sav_get_wstr(r, &o->file);
    if (o->type != OBJ_RECT && o->type != OBJ_IMAGE) o->type = OBJ_NONE;
    set_img_key(o);
    o->rect_rgb = (uint32_t)sav_get_i32(r);
    o->rect_x = sav_get_i32(r); o->rect_y = sav_get_i32(r); o->rect_w = sav_get_i32(r); o->rect_h = sav_get_i32(r);
    o->disp = sav_get_u8(r); o->clip_use = sav_get_u8(r); o->src_clip_use = sav_get_u8(r);
    o->wipe_copy = sav_get_u8(r); o->wipe_erase = sav_get_u8(r);
    o->layer = sav_get_i32(r); o->order = sav_get_i32(r);
    IntEvent *ev[] = GFX_OBJ_EVENTS(o);
    for (size_t k = 0; k < COUNT_OF(ev); k++) r_ev(r, ev[k]);
    for (int k = 0; k < TONE_CNT; k++) r_ev(r, &o->tone[k]);
    r_evlist(r, &o->x_rep, &o->nx_rep); r_evlist(r, &o->y_rep, &o->ny_rep);
    r_evlist(r, &o->z_rep, &o->nz_rep); r_evlist(r, &o->tr_rep, &o->ntr_rep);
    read_misc_v1(r, o);
    int nc = sav_get_count(r, GFX_OBJ_CHILD_MAX, 100);
    if (depth >= GFX_OBJ_DEPTH_MAX || !sav_ok(r)) nc = 0;
    if (!obj_children_resize(o, nc)) sav_fail(r);
    for (int k = 0; k < nc && k < o->nchild && sav_ok(r); k++) read_obj_v1(r, &o->child[k], depth + 1);
}

static void read_effect_v1(SavReader *r, Effect *e)
{
    r_ev(r, &e->x); r_ev(r, &e->y); r_ev(r, &e->z);
    for (int k = 0; k < TONE_CNT; k++) r_ev(r, &e->tone[k]);
    e->begin_order = sav_get_i32(r); e->end_order = sav_get_i32(r);
    e->begin_layer = sav_get_i32(r); e->end_layer = sav_get_i32(r);
    e->wipe_copy = sav_get_u8(r); e->wipe_erase = sav_get_u8(r);
}

/* ---- format 2 (GFX1): field presence masks against the defaults ---- */

/* Scalar fields in bit order (SAVE_FORMAT.md §9.2). All i32 on disk except bit 1 (str16). */
enum { OS_TYPE, OS_FILE, OS_RECT_RGB, OS_RECT_X, OS_RECT_Y, OS_RECT_W, OS_RECT_H, OS_DISP, OS_CLIP_USE,
       OS_SRC_CLIP_USE, OS_WIPE_COPY, OS_WIPE_ERASE, OS_LAYER, OS_ORDER, OS_WORLD, OS_BLEND, OS_TONECURVE,
       OS_CLICK_DISABLE, OS_LIGHT_NO, OS_FOG_USE, OS_MASK_NO, OS_CULLING, OS_ALPHA_TEST, OS_ALPHA_BLEND,
       OS_IS_BUTTON, OS_BTN_NO, OS_BTN_GROUP, OS_BTN_ACTION, OS_BTN_SE, OS_BTN_STATE, OS_COUNT };

/* Where each scalar lives in Obj and how it is stored (OS_FILE is the WStr, handled by the callers). */
enum { SK_NONE, SK_INT, SK_BOOL, SK_U32 };
typedef struct ScalarField { size_t off; int kind; } ScalarField;
#define SF(field, kind) { offsetof(Obj, field), kind }
static const ScalarField SCALARS[OS_COUNT] = {
    [OS_TYPE] = SF(type, SK_INT),           [OS_FILE] = { 0, SK_NONE },          [OS_RECT_RGB] = SF(rect_rgb, SK_U32),
    [OS_RECT_X] = SF(rect_x, SK_INT),       [OS_RECT_Y] = SF(rect_y, SK_INT),    [OS_RECT_W] = SF(rect_w, SK_INT),
    [OS_RECT_H] = SF(rect_h, SK_INT),       [OS_DISP] = SF(disp, SK_BOOL),       [OS_CLIP_USE] = SF(clip_use, SK_BOOL),
    [OS_SRC_CLIP_USE] = SF(src_clip_use, SK_BOOL), [OS_WIPE_COPY] = SF(wipe_copy, SK_BOOL),
    [OS_WIPE_ERASE] = SF(wipe_erase, SK_BOOL), [OS_LAYER] = SF(layer, SK_INT),   [OS_ORDER] = SF(order, SK_INT),
    [OS_WORLD] = SF(world, SK_INT),         [OS_BLEND] = SF(blend, SK_INT),      [OS_TONECURVE] = SF(tonecurve, SK_INT),
    [OS_CLICK_DISABLE] = SF(click_disable, SK_INT), [OS_LIGHT_NO] = SF(light_no, SK_INT),
    [OS_FOG_USE] = SF(fog_use, SK_INT),     [OS_MASK_NO] = SF(mask_no, SK_INT),  [OS_CULLING] = SF(culling, SK_INT),
    [OS_ALPHA_TEST] = SF(alpha_test, SK_INT), [OS_ALPHA_BLEND] = SF(alpha_blend, SK_INT),
    [OS_IS_BUTTON] = SF(is_button, SK_BOOL), [OS_BTN_NO] = SF(btn_no, SK_INT),   [OS_BTN_GROUP] = SF(btn_group, SK_INT),
    [OS_BTN_ACTION] = SF(btn_action, SK_INT), [OS_BTN_SE] = SF(btn_se, SK_INT),  [OS_BTN_STATE] = SF(btn_state, SK_INT),
};
#undef SF

static int32_t obj_scalar(const Obj *o, int k)
{
    ASSERT(k >= 0 && k < OS_COUNT);
    const unsigned char *f = (const unsigned char *)o + SCALARS[k].off;
    int iv; bool bv; uint32_t uv;
    switch (SCALARS[k].kind) {
    case SK_INT: memcpy(&iv, f, sizeof iv); return iv;
    case SK_BOOL: memcpy(&bv, f, sizeof bv); return bv;
    case SK_U32: memcpy(&uv, f, sizeof uv); return (int32_t)uv;
    default: return 0;
    }
}

static void obj_set_scalar(Obj *o, int k, int32_t v)
{
    ASSERT(k >= 0 && k < OS_COUNT);
    unsigned char *f = (unsigned char *)o + SCALARS[k].off;
    bool bv = v != 0;
    uint32_t uv = (uint32_t)v;
    if (k == OS_TYPE) v = (v == OBJ_RECT || v == OBJ_IMAGE) ? v : OBJ_NONE;
    switch (SCALARS[k].kind) {
    case SK_INT: memcpy(f, &v, sizeof v); break;
    case SK_BOOL: memcpy(f, &bv, sizeof bv); break;
    case SK_U32: memcpy(f, &uv, sizeof uv); break;
    default: break;
    }
}

static const Obj *default_obj(void)
{
    static Obj d;   /* obj_init of a zeroed object allocates nothing */
    static bool init;
    if (!init) { memset(&d, 0, sizeof d); obj_init(&d); init = true; }
    return &d;
}

static bool obj_is_default(const Obj *o)
{
    const Obj *d = default_obj();
    if (o->nchild || o->nx_rep || o->ny_rep || o->nz_rep || o->ntr_rep || o->file.len) return false;
    for (int k = 0; k < OS_COUNT; k++) if (k != OS_FILE && obj_scalar(o, k) != obj_scalar(d, k)) return false;
    const IntEvent *ev[] = GFX_OBJ_EVENTS(o), *dv[] = GFX_OBJ_EVENTS(d);
    for (int k = 0; k < GFX_OBJ_EVENT_CNT; k++) if (!ev_same(ev[k], dv[k])) return false;
    for (int k = 0; k < TONE_CNT; k++) if (!ev_same(&o->tone[k], &d->tone[k])) return false;
    return true;
}

static void write_scalars2(SavWriter *w, const Obj *o, const Obj *d)
{
    uint32_t sm = 0;
    for (int k = 0; k < OS_COUNT; k++)
        if (k == OS_FILE ? o->file.len > 0 : obj_scalar(o, k) != obj_scalar(d, k)) sm |= 1u << k;
    sav_put_u32(w, sm);
    for (int k = 0; k < OS_COUNT; k++) {
        if (!(sm >> k & 1)) continue;
        if (k == OS_FILE) sav_put_wstr(w, &o->file); else sav_put_i32(w, obj_scalar(o, k));
    }
}

/* Events 0..24 = GFX_OBJ_EVENTS, 25..35 = tone[0..10], as two u32 masks. */
static const IntEvent *obj_event_at(const Obj *o, int b)
{
    const IntEvent *ev[] = GFX_OBJ_EVENTS(o);
    return b < GFX_OBJ_EVENT_CNT ? ev[b] : &o->tone[b - GFX_OBJ_EVENT_CNT];
}

static void write_events2(SavWriter *w, const Obj *o, const Obj *d)
{
    uint32_t em[2] = { 0, 0 };
    for (int b = 0; b < GFX_OBJ_EVENT_CNT + TONE_CNT; b++)
        if (!ev_same(obj_event_at(o, b), obj_event_at(d, b))) em[b / 32] |= 1u << (b % 32);
    sav_put_u32(w, em[0]); sav_put_u32(w, em[1]);
    for (int b = 0; b < GFX_OBJ_EVENT_CNT + TONE_CNT; b++)
        if (em[b / 32] >> (b % 32) & 1) w_ev_diff(w, obj_event_at(o, b), obj_event_at(d, b));
}

static void write_reps2(SavWriter *w, const Obj *o)
{
    const IntEvent *const reps[REP_LISTS] = { o->x_rep, o->y_rep, o->z_rep, o->tr_rep };
    const int nrep[REP_LISTS] = { o->nx_rep, o->ny_rep, o->nz_rep, o->ntr_rep };
    uint32_t rm = 0;
    for (int k = 0; k < REP_LISTS; k++) if (nrep[k] > 0) rm |= 1u << k;
    sav_put_u32(w, rm);
    for (int k = 0; k < REP_LISTS; k++) {
        if (!(rm >> k & 1)) continue;
        sav_put_u32(w, (uint32_t)nrep[k]);
        for (int i = 0; i < nrep[k]; i++) w_ev(w, &reps[k][i]);
    }
}

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void write_obj2(SavWriter *w, const Obj *o, int depth)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    const Obj *d = default_obj();
    write_scalars2(w, o, d);
    write_events2(w, o, d);
    write_reps2(w, o);
    sav_put_u32(w, (uint32_t)o->nchild);
    for (int k = 0; k < o->nchild; k++) write_obj2(w, &o->child[k], depth + 1);
}

static void read_scalars2(SavReader *r, Obj *o)
{
    uint32_t sm = sav_get_u32(r);
    for (int k = 0; k < 32 && sav_ok(r); k++) {
        if (!(sm >> k & 1)) continue;
        if (k == OS_FILE) sav_get_wstr(r, &o->file);
        else if (k < OS_COUNT) obj_set_scalar(o, k, sav_get_i32(r));
        else (void)sav_get_i32(r);   /* future scalar fields are i32 */
    }
    set_img_key(o);
}

static void read_events2(SavReader *r, Obj *o)
{
    uint32_t em[2];
    em[0] = sav_get_u32(r);
    em[1] = sav_get_u32(r);
    IntEvent *ev[] = GFX_OBJ_EVENTS(o);
    for (int b = 0; b < 64 && sav_ok(r); b++) {
        if (!(em[b / 32] >> (b % 32) & 1)) continue;
        if (b < GFX_OBJ_EVENT_CNT) r_ev_diff(r, ev[b]);
        else if (b < GFX_OBJ_EVENT_CNT + TONE_CNT) r_ev_diff(r, &o->tone[b - GFX_OBJ_EVENT_CNT]);
        else r_ev_skip(r);
    }
}

static void read_reps2(SavReader *r, Obj *o)
{
    IntEvent **reps[REP_LISTS] = { &o->x_rep, &o->y_rep, &o->z_rep, &o->tr_rep };
    int *nrep[REP_LISTS] = { &o->nx_rep, &o->ny_rep, &o->nz_rep, &o->ntr_rep };
    uint32_t rm = sav_get_u32(r);
    if (rm >> REP_LISTS) sav_fail(r);   /* no other rep lists exist */
    for (int k = 0; k < REP_LISTS && sav_ok(r); k++) if (rm >> k & 1) r_evlist(r, reps[k], nrep[k]);
}

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void read_obj2(SavReader *r, Obj *o, int depth)   /* o must be freshly initialised */
{
    read_scalars2(r, o);
    read_events2(r, o);
    read_reps2(r, o);
    int nc = sav_get_count(r, GFX_OBJ_CHILD_MAX, 16);
    if (depth >= GFX_OBJ_DEPTH_MAX && nc) sav_fail(r);
    if (!sav_ok(r)) return;
    if (!obj_children_resize(o, nc)) sav_fail(r);
    for (int k = 0; k < nc && k < o->nchild && sav_ok(r); k++) read_obj2(r, &o->child[k], depth + 1);
}

/* Effect: u32 event mask (x, y, z, tone[0..10]) + u32 scalar mask (begin/end order, begin/end layer, wipe_copy,
 * wipe_erase). */
static IntEvent *effect_event(Effect *e, int k)
{ return k == 0 ? &e->x : k == 1 ? &e->y : k == 2 ? &e->z : &e->tone[k - 3]; }
static const IntEvent *effect_event_c(const Effect *e, int k)
{
    return k == 0 ? &e->x : k == 1 ? &e->y : k == 2 ? &e->z : &e->tone[k - 3];
}

static void effect_scalars(const Effect *e, int32_t v[EFFECT_SCALARS])
{
    v[0] = e->begin_order; v[1] = e->end_order; v[2] = e->begin_layer; v[3] = e->end_layer;
    v[4] = e->wipe_copy; v[5] = e->wipe_erase;
}

static void write_effect2(SavWriter *w, const Effect *e)
{
    Effect d;
    gfx_effect_init(&d);
    uint32_t em = 0;
    for (int k = 0; k < EFFECT_EVENTS; k++) if (!ev_same(effect_event_c(e, k), effect_event_c(&d, k))) em |= 1u << k;
    sav_put_u32(w, em);
    for (int k = 0; k < EFFECT_EVENTS; k++) if (em >> k & 1) w_ev_diff(w, effect_event_c(e, k), effect_event_c(&d, k));
    int32_t sv[EFFECT_SCALARS], sd[EFFECT_SCALARS];
    effect_scalars(e, sv);
    effect_scalars(&d, sd);
    uint32_t sm = 0;
    for (int k = 0; k < EFFECT_SCALARS; k++) if (sv[k] != sd[k]) sm |= 1u << k;
    sav_put_u32(w, sm);
    for (int k = 0; k < EFFECT_SCALARS; k++) if (sm >> k & 1) sav_put_i32(w, sv[k]);
}

static void read_effect2(SavReader *r, Effect *e)   /* e holds the defaults already */
{
    uint32_t em = sav_get_u32(r);
    for (int k = 0; k < 32 && sav_ok(r); k++) {
        if (!(em >> k & 1)) continue;
        if (k < EFFECT_EVENTS) r_ev_diff(r, effect_event(e, k)); else r_ev_skip(r);
    }
    uint32_t sm = sav_get_u32(r);
    int32_t sv[32] = { 0 };
    for (int k = 0; k < 32 && sav_ok(r); k++) if (sm >> k & 1) sv[k] = sav_get_i32(r);
    if (sm & 1) e->begin_order = sv[0];
    if (sm & 2) e->end_order = sv[1];
    if (sm & 4) e->begin_layer = sv[2];
    if (sm & 8) e->end_layer = sv[3];
    if (sm & 16) e->wipe_copy = sv[4] != 0;
    if (sm & 32) e->wipe_erase = sv[5] != 0;
}

/* ---- chunk ---- */

static bool obj_saved(int idx)   /* #OBJECT.000-099.SAVE = 1 */
{
    char key[40];
    if (!str_fmt(key, sizeof key, "OBJECT.%03d.SAVE", idx)) return false;   /* idx < GFX_OBJ_CNT: always fits */
    return gx_int(key, 0, idx < 100) != 0;
}

static bool obj_to_save(int s, int i) { return obj_saved(i) && !obj_is_default(gfx_obj(s, i)); }

void gfx_save(SavWriter *w)
{
    sav_put_chunk(w, FOURCC('G', 'F', 'X', '1'));
    int n = 0;
    for (int s = GFX_STAGE_BACK; s <= GFX_STAGE_FRONT; s++)
        for (int i = 0; i < GFX_OBJ_CNT; i++) n += obj_to_save(s, i);
    sav_put_u32(w, (uint32_t)n);
    for (int s = GFX_STAGE_BACK; s <= GFX_STAGE_FRONT; s++)
        for (int i = 0; i < GFX_OBJ_CNT; i++) {
            if (!obj_to_save(s, i)) continue;
            sav_put_u32(w, (uint32_t)s); sav_put_u32(w, (uint32_t)i);
            write_obj2(w, gfx_obj(s, i), 0);
        }
    for (int i = 0; i < GFX_EFFECT_CNT; i++) write_effect2(w, gfx_effect(i));
    write_effect2(w, gfx_screen_tone());
}

static bool gfx_load_v1(SavReader *r)
{
    if (sav_get_i32(r) != GFX_SAVE_VERSION) return false;
    for (int s = GFX_STAGE_BACK; s <= GFX_STAGE_FRONT && sav_ok(r); s++) {
        int n = sav_get_count(r, GFX_OBJ_CNT, 8);
        for (int k = 0; k < n && sav_ok(r); k++) {
            int i = sav_get_i32(r);
            Obj *o = gfx_obj(s, i);
            if (!o) return false;
            read_obj_v1(r, o, 0);
            o->stage = s; o->idx = i;
        }
    }
    for (int i = 0; i < GFX_EFFECT_CNT; i++) read_effect_v1(r, gfx_effect(i));
    read_effect_v1(r, gfx_screen_tone());
    return sav_ok(r);
}

static bool gfx_load_v2(SavReader *r)
{
    int n = sav_get_count(r, GFX_STAGE_CNT * GFX_OBJ_CNT, 28);
    for (int k = 0; k < n && sav_ok(r); k++) {
        uint32_t s = sav_get_u32(r), i = sav_get_u32(r);
        Obj *o = (s <= GFX_STAGE_FRONT && i < GFX_OBJ_CNT) ? gfx_obj((int)s, (int)i) : NULL;
        if (!o || !sav_ok(r)) return false;
        obj_init(o);
        read_obj2(r, o, 0);
        o->stage = (int)s; o->idx = (int)i;
    }
    for (int i = 0; i < GFX_EFFECT_CNT; i++) read_effect2(r, gfx_effect(i));
    read_effect2(r, gfx_screen_tone());
    return sav_ok(r);
}

bool gfx_load(SavReader *r)
{
    bool v2 = sav_find_chunk(r, FOURCC('G', 'F', 'X', '1'));
    if (!v2 && !(sav_legacy(r) && sav_find_chunk(r, FOURCC('G', 'F', 'X', '0')))) return false;
    gfx_reset_all();
    bool ok = v2 ? gfx_load_v2(r) : gfx_load_v1(r);
    if (!ok) { gfx_reset_all(); sav_fail(r); }   /* a damaged chunk leaves an empty screen, not a half-built one */
    return ok;
}
