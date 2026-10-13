/* gfx/gfx_elm.c — roots 7, 23, 33, 37, 38 (not .6), 70, 73, 103, 109; object/event sub-elements;
 * procs 0x23/0x24/0x26 (GFX §1, §3, §4, §5, §6, §8; TXT §3.9; ENGINE_SPEC §2.5, WP3). */
#include "core/log.h"
#include "core/str.h"
#include "gfx/button.h"
#include "gfx/gfx.h"
#include "gfx/pixel.h"
#include "gfx/render.h"
#include "vm/elements.h"
#include "vm/proc.h"
#include "vm/vm.h"
#include <limits.h>
#include <stddef.h>
#include <string.h>

/* Chain of the gfx_elm_global call in progress, for procs that store an event chain. Set on entry and restored on
 * exit, so a nested dispatch (a reference argument re-dispatches) sees its own chain and nothing dangles after. */
typedef struct { int root; const int *rest; } ElmChain;
static ElmChain g_call;
static GfxStageMwndFn g_stage_mwnd;

void gfx_set_stage_mwnd_handler(GfxStageMwndFn fn) { g_stage_mwnd = fn; }

/* Unused elements answer with the default value; each (kind, id) is logged once (first STUB_SEEN_MAX kinds). */
enum { STUB_SEEN_MAX = 64, EVENT_CHAIN_MAX = (int)COUNT_OF(((Proc *)0)->chain) };
static void stub_once(const char *what, int id, ElmCall *c)
{
    static struct { const char *what; int id; } seen[STUB_SEEN_MAX];
    static int nseen;
    bool known = false;
    for (int k = 0; k < nseen && !known; k++) known = seen[k].what == what && seen[k].id == id;
    if (!known && nseen < STUB_SEEN_MAX) {
        seen[nseen].what = what;
        seen[nseen].id = id;
        nseen++;
        log_msg(LOG_DEBUG, "gfx: unused element %s %d (stub)", what, id);
    }
    elm_push_default(c);
}

static int32_t rhs_int(const ElmCall *c) { return (c->arg && c->nall > 0) ? c->arg[0].i : 0; }
/* Object handlers read their arguments form-agnostically, as the exe does (elm_arg_raw_int / elm_arg_raw_str). */

/* ------------------------------------------------------------ IntEvent element (FUN_00661040) */

static void push_event_wait(const int *rest_end_of_event, bool key)
{
    Proc p = { .type = PROC_EVENT_WAIT };
    ASSERT(g_call.rest != NULL);                     /* only reached from inside gfx_elm_global */
    ptrdiff_t used = rest_end_of_event - g_call.rest;   /* root + rest up to and including the event element */
    int n = used < 0 ? 1 : used + 2 > EVENT_CHAIN_MAX ? EVENT_CHAIN_MAX : (int)used + 2;
    p.chain[0] = g_call.root;
    for (int k = 1; k < n; k++) p.chain[k] = g_call.rest[k - 1];
    p.nchain = n;
    p.key_skip = key; p.push_result = key;           /* wait_key: local_c8 = local_c6 = 1 */
    proc_push(&p);
}

static ElmResult ev_elm(IntEvent *e, const int *rest, int n, ElmCall *c, int clamp)
{
    if (n == 0) {   /* the event as a value (not used by the game) */
        if (c->op == OP_ASSIGN) ievent_assign(e, rhs_int(c));
        else vm_push_int(e->value);
        return ELM_OK;
    }
    const Arg *st = elm_arg_named(c, 0);
    switch (rest[0]) {
    case ELM_INTEVENT_SET:
    case ELM_INTEVENT_SET_REAL: /* set / set_real(value, total, delay, speed, start=) */
        ievent_set(e, elm_arg_raw_int(c, 0, 0), elm_arg_raw_int(c, 1, 0), elm_arg_raw_int(c, 2, 0),
                   elm_arg_raw_int(c, 3, 0), st != NULL, st ? st->i : 0, rest[0] == ELM_INTEVENT_SET_REAL);
        if (clamp) { e->value = px_clamp255(e->value); e->end = px_clamp255(e->end); }
        return ELM_OK;
    case ELM_INTEVENT_LOOP:
    case ELM_INTEVENT_LOOP_REAL:
    case ELM_INTEVENT_TURN:
    case ELM_INTEVENT_TURN_REAL: /* loop / loop_real / turn / turn_real(start, end, total, delay, speed) */
        ievent_loop(e, elm_arg_raw_int(c, 0, 0), elm_arg_raw_int(c, 1, 0), elm_arg_raw_int(c, 2, 0),
                    elm_arg_raw_int(c, 3, 0), elm_arg_raw_int(c, 4, 0),
                    (rest[0] == ELM_INTEVENT_LOOP || rest[0] == ELM_INTEVENT_LOOP_REAL) ? 1 : 2,
                    rest[0] >= ELM_INTEVENT_LOOP_REAL); /* the *_real variants */
        return ELM_OK;
    case ELM_INTEVENT_END: ievent_end(e); return ELM_OK;
    case ELM_INTEVENT_WAIT:
    case ELM_INTEVENT_WAIT_KEY: /* wait / wait_key: proc 0x26 */
        push_event_wait(rest - 1, rest[0] == ELM_INTEVENT_WAIT_KEY);
        return ELM_OK;
    case ELM_INTEVENT_CHECK: vm_push_int(ievent_active(e)); return ELM_OK;              /* check [I] 1 while running */
    case ELM_INTEVENT_X_SET: ievent_assign(e, elm_arg_raw_int(c, 0, 0)); return ELM_OK; /* __set */
    case ELM_INTEVENT_GET_EVENT_VALUE: vm_push_int(e->cur); return ELM_OK;
    case ELM_INTEVENT_YURE:
    case ELM_INTEVENT_YURE_REAL: stub_once("intevent", rest[0], c); return ELM_OK;
    default: return ELM_UNKNOWN;
    }
}

/* ------------------------------------------------------------ int list of IntEvents (rep lists, VM §6.1 subset) */

static ElmResult replist_elm(IntEvent **l, int *cnt, const int *rest, int n, ElmCall *c)
{
    if (n >= 2 && rest[0] == -1) {
        int k = rest[1];
        if (k < 0 || k >= *cnt) {
            log_msg(LOG_WARN, "gfx: rep list index %d out of range (%d)", k, *cnt);
            if (c->op == OP_PROPERTY) vm_push_int(0);
            return ELM_OK;
        }
        if (n > 2) return ELM_UNKNOWN;
        if (c->op == OP_ASSIGN) { (*l)[k].value = (*l)[k].cur = rhs_int(c); }
        else vm_push_int((*l)[k].value);
        return ELM_OK;
    }
    if (n < 1) return ELM_UNKNOWN;
    switch (rest[0]) {
    case 2: (void)obj_rep_resize(l, cnt, elm_arg_raw_int(c, 0, 0)); return ELM_OK;  /* resize (a refusal is logged) */
    case 9: vm_push_int(*cnt); return ELM_OK;                            /* size */
    case 10: (void)obj_rep_resize(l, cnt, 0); return ELM_OK;             /* init: never fails */
    case 1:                                                              /* sets(start, v...) */
        for (int j = 1; j < c->npos; j++) {
            int64_t k = (int64_t)elm_arg_raw_int(c, 0, 0) + j - 1;
            if (k >= 0 && k < *cnt) ievent_assign(&(*l)[k], elm_arg_raw_int(c, j, 0));
        }
        return ELM_OK;
    case 8: {                                                            /* clear(start, end[, v]) */
        int v = c->npos >= 3 ? elm_arg_raw_int(c, 2, 0) : 0;
        int lo = elm_arg_raw_int(c, 0, 0) < 0 ? 0 : elm_arg_raw_int(c, 0, 0),
            hi = elm_arg_raw_int(c, 1, -1) >= *cnt ? *cnt - 1 : elm_arg_raw_int(c, 1, -1);
        for (int k = lo; k <= hi; k++) ievent_assign(&(*l)[k], v);      /* only the part inside the list */
        return ELM_OK;
    }
    default: return ELM_UNKNOWN;
    }
}

static ElmResult evlist_elm(IntEvent **l, int *cnt, const int *rest, int n, ElmCall *c)
{
    if (n >= 2 && rest[0] == -1) {
        int k = rest[1];
        if (k < 0 || k >= *cnt) {
            log_msg(LOG_WARN, "gfx: rep event index %d out of range", k);
            elm_push_default(c);
            return ELM_OK;
        }
        return ev_elm(&(*l)[k], rest + 2, n - 2, c, 0);
    }
    if (n >= 1 && rest[0] == 1) {
        (void)obj_rep_resize(l, cnt, elm_arg_raw_int(c, 0, 0));
        return ELM_OK;
    } /* logged */
    return ELM_UNKNOWN;
}

/* ------------------------------------------------------------ object (FUN_0065dbd0) */

static IntEvent *obj_prop_event(Obj *o, int id, int *clamp)
{
    *clamp = 0;
    switch (id) {
    case ELM_OBJECT_START_GAN: return &o->patno;
    case ELM_OBJECT_X: return &o->x;
    case ELM_OBJECT_Y: return &o->y;
    case ELM_OBJECT_Z: return &o->z;
    case ELM_OBJECT_CENTER_X: return &o->center_x;
    case ELM_OBJECT_CENTER_Y: return &o->center_y;
    case ELM_OBJECT_CENTER_Z: return &o->center_z;
    case ELM_OBJECT_CENTER_REP_X: return &o->center_rep_x;
    case ELM_OBJECT_CENTER_REP_Y: return &o->center_rep_y;
    case ELM_OBJECT_CENTER_REP_Z: return &o->center_rep_z;
    case ELM_OBJECT_SCALE_X: return &o->scale_x;
    case ELM_OBJECT_SCALE_Y: return &o->scale_y;
    case ELM_OBJECT_SCALE_Z: return &o->scale_z;
    case ELM_OBJECT_ROTATE_X: return &o->rotate_x;
    case ELM_OBJECT_ROTATE_Y: return &o->rotate_y;
    case ELM_OBJECT_ROTATE_Z: return &o->rotate_z;
    case ELM_OBJECT_CLIP_LEFT: return &o->clip_l;
    case ELM_OBJECT_CLIP_TOP: return &o->clip_t;
    case ELM_OBJECT_CLIP_RIGHT: return &o->clip_r;
    case ELM_OBJECT_CLIP_BOTTOM: return &o->clip_b;
    case ELM_OBJECT_SRC_CLIP_LEFT: return &o->src_l;
    case ELM_OBJECT_SRC_CLIP_TOP: return &o->src_t;
    case ELM_OBJECT_SRC_CLIP_RIGHT: return &o->src_r;
    case ELM_OBJECT_SRC_CLIP_BOTTOM: return &o->src_b;
    case ELM_OBJECT_TR: *clamp = 1; return &o->tr; /* FUN_00623d00 clamps 0..255 (ENGINE_SPEC §1.3 #11) */
    case ELM_OBJECT_MONO:
    case ELM_OBJECT_REVERSE:
    case ELM_OBJECT_BRIGHT:
    case ELM_OBJECT_DARK:
    case ELM_OBJECT_COLOR_R:
    case ELM_OBJECT_COLOR_G:
    case ELM_OBJECT_COLOR_B: *clamp = 1; return &o->tone[TONE_MONO + id - 28];
    case ELM_OBJECT_COLOR_RATE: *clamp = 1; return &o->tone[TONE_RATE];
    case ELM_OBJECT_COLOR_ADD_R:
    case ELM_OBJECT_COLOR_ADD_G:
    case ELM_OBJECT_COLOR_ADD_B: *clamp = 1; return &o->tone[TONE_ADD_R + id - 57];
    default: return NULL;
    }
}

static IntEvent *obj_event(Obj *o, int id, int *clamp)   /* event ids (jump table 0x660328) */
{
    static const short map[][2] = {
        { 90, 1 }, { 51, 3 }, { 64, 4 }, { 65, 5 }, { 77, 6 }, { 78, 7 }, { 79, 8 }, { 80, 9 }, { 81, 10 }, { 82, 11 },
        { 67, 12 }, { 68, 13 }, { 66, 14 }, { 69, 15 }, { 70, 16 }, { 71, 17 }, { 72, 27 },
        { 73, 28 }, { 74, 29 }, { 75, 30 }, { 76, 31 }, { 87, 32 }, { 88, 33 }, { 89, 34 }, { 83, 23 },
        { 84, 57 }, { 85, 58 }, { 86, 59 }, { 105, 19 }, { 106, 20 }, { 107, 21 }, { 108, 22 },
        { 154, 150 }, { 155, 151 }, { 156, 152 }, { 157, 153 },
    };
    for (size_t k = 0; k < sizeof map / sizeof map[0]; k++)
        if (map[k][0] == id) return obj_prop_event(o, map[k][1], clamp);
    return NULL;
}

static int *obj_prop_int(Obj *o, int id)
{
    switch (id) {
    case ELM_OBJECT_LAYER: return &o->layer;
    case ELM_OBJECT_ORDER: return &o->order;
    case ELM_OBJECT_WORLD: return &o->world;
    case ELM_OBJECT_BLEND: return &o->blend;
    case ELM_OBJECT_TONECURVE_NO: return &o->tonecurve;
    case ELM_OBJECT_CLICK_DISABLE: return &o->click_disable;
    case ELM_OBJECT_LIGHT_NO: return &o->light_no;
    case ELM_OBJECT_FOG_USE: return &o->fog_use;
    case ELM_OBJECT_MASK_NO: return &o->mask_no;
    case ELM_OBJECT_CULLING: return &o->culling;
    case ELM_OBJECT_ALPHA_TEST: return &o->alpha_test;
    case ELM_OBJECT_ALPHA_BLEND: return &o->alpha_blend;
    default: return NULL;
    }
}

static bool *obj_prop_bool(Obj *o, int id)
{
    switch (id) {
    case ELM_OBJECT_LOAD_GAN: return &o->disp;
    case ELM_OBJECT_CLIP_USE: return &o->clip_use;
    case ELM_OBJECT_SRC_CLIP_USE: return &o->src_clip_use;
    case ELM_OBJECT_WIPE_COPY: return &o->wipe_copy;
    case ELM_OBJECT_WIPE_ERASE: return &o->wipe_erase;
    default: return NULL;
    }
}

static void set3(IntEvent *a, IntEvent *b, IntEvent *c3, const ElmCall *c)
{
    ievent_assign(a, elm_arg_raw_int(c, 0, 0));
    ievent_assign(b, elm_arg_raw_int(c, 1, 0));
    if (c->npos >= 3) ievent_assign(c3, elm_arg_raw_int(c, 2, 0));
}

static void set_clip4(bool *use, IntEvent *l, IntEvent *t, IntEvent *r, IntEvent *b, const ElmCall *c)
{
    *use = elm_arg_raw_int(c, 0, 0) != 0;
    ievent_assign(l, elm_arg_raw_int(c, 1, 0)); ievent_assign(t, elm_arg_raw_int(c, 2, 0));
    ievent_assign(r, elm_arg_raw_int(c, 3, 0)); ievent_assign(b, elm_arg_raw_int(c, 4, 0));
}

/* obj.93.<sub>: the child list of an object (element access obj.93.-1.<i> is walked in obj_elm). */
static ElmResult children_elm(Obj *owner, const int *rest, int n, ElmCall *c)
{
    if (n >= 1 && rest[0] == 3) { vm_push_int(owner->nchild); return ELM_OK; }          /* get_size */
    if (n >= 1 && (rest[0] == 4 || rest[0] == 1)) {
        (void)obj_children_resize(owner, elm_arg_raw_int(c, 0, 0));
        return ELM_OK;
    }
    return ELM_UNKNOWN;
}

/* obj.<id>.<...>: event, rep list, child list or button sub-elements. */
static ElmResult obj_sub_elm(Obj *o, int id, const int *rest, int n, ElmCall *c)
{
    int clamp;
    IntEvent *ev = obj_event(o, id, &clamp);
    if (ev) return ev_elm(ev, rest, n, c, clamp);
    switch (id) {
    case ELM_OBJECT_X_REP: return replist_elm(&o->x_rep, &o->nx_rep, rest, n, c);
    case ELM_OBJECT_Y_REP: return replist_elm(&o->y_rep, &o->ny_rep, rest, n, c);
    case ELM_OBJECT_Z_REP: return replist_elm(&o->z_rep, &o->nz_rep, rest, n, c);
    case ELM_OBJECT_TR_REP: return replist_elm(&o->tr_rep, &o->ntr_rep, rest, n, c);
    case ELM_OBJECT_X_REP_EVE: return evlist_elm(&o->x_rep, &o->nx_rep, rest, n, c);
    case ELM_OBJECT_Y_REP_EVE: return evlist_elm(&o->y_rep, &o->ny_rep, rest, n, c);
    case ELM_OBJECT_Z_REP_EVE: return evlist_elm(&o->z_rep, &o->nz_rep, rest, n, c);
    case ELM_OBJECT_TR_REP_EVE: return evlist_elm(&o->tr_rep, &o->ntr_rep, rest, n, c);
    case ELM_OBJECT_CHILD: return children_elm(o, rest, n, c);
    case ELM_OBJECT_SET_BUTTON: return btn_elm_obj(o, rest, n, c);
    case ELM_OBJECT_FRAME_ACTION:
    case ELM_OBJECT_FRAME_ACTION_CH:
    case ELM_OBJECT_ALL_EVE:
    case ELM_OBJECT_F: stub_once("object", id, c); return ELM_OK; /* frame actions, all_eve, f */
    default: return ELM_UNKNOWN;
    }
}

/* obj.<id> as a property (get/set). False when id is not a property. */
static bool obj_prop_elm(Obj *o, int id, ElmCall *c)
{
    int clamp;
    IntEvent *ev = obj_prop_event(o, id, &clamp);
    bool *b = obj_prop_bool(o, id);
    int *pi = obj_prop_int(o, id);
    if (ev) {
        if (c->op == OP_ASSIGN) { int v = rhs_int(c); ev->value = ev->cur = clamp ? px_clamp255(v) : v; }
        else vm_push_int(ev->value);
    } else if (b) {
        if (c->op == OP_ASSIGN) *b = rhs_int(c) != 0; else vm_push_int(*b);
    } else if (pi) {
        if (c->op == OP_ASSIGN) *pi = rhs_int(c); else vm_push_int(*pi);
    } else if (c->op == OP_PROPERTY && (obj_event(o, id, &clamp) || id == ELM_OBJECT_X_REP || id == ELM_OBJECT_Y_REP ||
                                        id == ELM_OBJECT_CHILD)) {
        c->ret_obj = o;   /* element-typed property used as a reference */
    } else {
        return false;
    }
    return true;
}

/* Commands of the object element that the game never uses: gan/string/number/movie/weather/emote/button-state
 * (ENGINE_SPEC §1.5); they answer with the default value. */
static bool obj_cmd_unused(int id)
{
    static const unsigned char unused[] = {
        0, 1, 26, 39, 43, 45, 47, 60, 98, 99, 100, 101, 102, 103, 104, 116, 117, 119, 120, 121, 122, 125, 126, 127,
        128, 129, 130, 131, 132, 133, 134, 135, 136, 137, 138, 142, 143, 162, 163, 165, 166, 167, 170, 171, 172, 173,
        177, 178, 179, 180, 181, 182, 183, 184, 185, 186, 187,
    };
    for (size_t k = 0; k < COUNT_OF(unused); k++) if (unused[k] == id) return true;
    return false;
}

/* Button sub-commands handled by WP6 (obj.<id> with id = sub code). */
static bool obj_cmd_button(int id)
{
    static const unsigned char subs[] = { 61, 95, 96, 97, 118, 123, 124, 164, 175, 176, 188, 189, 190, 191 };
    for (size_t k = 0; k < COUNT_OF(subs); k++) if (subs[k] == id) return true;
    return false;
}

static void obj_cmd_create(Obj *o, ElmCall *c)   /* create(name[, disp[, x, y[, patno]]]) (case 0x26) */
{
    obj_create_image(o, elm_arg_raw_str(c, 0));
    if (c->npos >= 2) o->disp = elm_arg_raw_int(c, 1, 0) != 0;
    if (c->npos >= 4) {
        ievent_assign(&o->x, elm_arg_raw_int(c, 2, 0));
        ievent_assign(&o->y, elm_arg_raw_int(c, 3, 0));
    }
    if (c->npos >= 5) ievent_assign(&o->patno, elm_arg_raw_int(c, 4, 0));
}

static void obj_cmd_create_rect(Obj *o, ElmCall *c)   /* create_rect(x, y, w, h, r, g, b, a[, disp[, px, py]]) */
{
    obj_create_rect(o, elm_arg_raw_int(c, 0, 0), elm_arg_raw_int(c, 1, 0), elm_arg_raw_int(c, 2, 0),
                    elm_arg_raw_int(c, 3, 0), elm_arg_raw_int(c, 4, 0), elm_arg_raw_int(c, 5, 0),
                    elm_arg_raw_int(c, 6, 0), elm_arg_raw_int(c, 7, 255)); /* case 0x28 + FUN_005f3ae0 */
    if (c->npos >= 9) o->disp = elm_arg_raw_int(c, 8, 0) != 0;
    if (c->npos >= 11) {
        ievent_assign(&o->x, elm_arg_raw_int(c, 9, 0));
        ievent_assign(&o->y, elm_arg_raw_int(c, 10, 0));
    }
}

static void obj_cmd_copy_from(Obj *o, const ElmCall *c)   /* create_copy_from(&obj) */
{
    const Arg *a = (c->arg && c->npos > 0) ? &c->arg[0] : NULL;
    Obj *src = a ? gfx_resolve_obj(a->chain, a->nchain) : NULL;
    if (src) obj_copy(o, src);
    else log_msg(LOG_WARN, "gfx: obj.copy: source does not resolve");
}

static int obj_size(Obj *o, int id, const ElmCall *c)   /* get_size_x/y([patno]) */
{
    if (o->type == OBJ_RECT) return id == ELM_OBJECT_GET_SIZE_X ? o->rect_w : o->rect_h;
    const Image *im = o->type == OBJ_IMAGE && o->img_key
                          ? img_get(o->img_key, c->npos >= 1 ? elm_arg_raw_int(c, 0, 0) : o->patno.value)
                          : NULL;
    return im ? (id == ELM_OBJECT_GET_SIZE_X ? im->w : im->h) : 0;
}

static void obj_cmd_change_file(Obj *o, const ElmCall *c)   /* change_file(name): new image, params kept */
{
    WStr nm = { 0 };
    ws_copy(&nm, elm_arg_raw_str(c, 0));   /* the argument may alias o->file */
    obj_free(o);
    o->type = OBJ_IMAGE;
    obj_set_image(o, &nm);
    ws_free(&nm);
}

static ElmResult obj_cmd_elm(Obj *o, const int *rest, ElmCall *c)
{
    int id = rest[0];
    switch (id) {
    case ELM_OBJECT_INIT: obj_init(o); return ELM_OK;
    case ELM_OBJECT_FREE: obj_free(o); return ELM_OK;
    case ELM_OBJECT_INIT_PARAM: obj_init_param(o); return ELM_OK;
    case ELM_OBJECT_CREATE: obj_cmd_create(o, c); return ELM_OK;
    case ELM_OBJECT_CREATE_RECT: obj_cmd_create_rect(o, c); return ELM_OK;
    case ELM_OBJECT_CREATE_COPY_FROM: obj_cmd_copy_from(o, c); return ELM_OK;
    case ELM_OBJECT_SET_BUTTON: return btn_elm_obj(o, rest + 1, 0, c);
    case ELM_OBJECT_SET_POS: set3(&o->x, &o->y, &o->z, c); return ELM_OK;
    case ELM_OBJECT_SET_SCALE: set3(&o->scale_x, &o->scale_y, &o->scale_z, c); return ELM_OK;
    case ELM_OBJECT_SET_ROTATE: set3(&o->rotate_x, &o->rotate_y, &o->rotate_z, c); return ELM_OK;
    case ELM_OBJECT_SET_CENTER: set3(&o->center_x, &o->center_y, &o->center_z, c); return ELM_OK;
    case ELM_OBJECT_SET_CENTER_REP: set3(&o->center_rep_x, &o->center_rep_y, &o->center_rep_z, c); return ELM_OK;
    case ELM_OBJECT_SET_CLIP: set_clip4(&o->clip_use, &o->clip_l, &o->clip_t, &o->clip_r, &o->clip_b, c); return ELM_OK;
    case ELM_OBJECT_SET_SRC_CLIP:
        set_clip4(&o->src_clip_use, &o->src_l, &o->src_t, &o->src_r, &o->src_b, c);
        return ELM_OK;
    case ELM_OBJECT_GET_PAT_CNT: vm_push_int(obj_pat_count(o)); return ELM_OK;
    case ELM_OBJECT_GET_TYPE: vm_push_int(o->type); return ELM_OK;
    case ELM_OBJECT_EXIST_TYPE: vm_push_int(o->type != OBJ_NONE); return ELM_OK;
    case ELM_OBJECT_GET_FILE_NAME: vm_push_str(&o->file); return ELM_OK;
    case ELM_OBJECT_GET_SIZE_X:
    case ELM_OBJECT_GET_SIZE_Y: vm_push_int(obj_size(o, id, c)); return ELM_OK;
    case ELM_OBJECT_CHANGE_FILE: obj_cmd_change_file(o, c); return ELM_OK;
    default: break;
    }
    if (obj_cmd_button(id)) { int sub = id; return btn_elm_obj(o, &sub, 1, c); }   /* rest[0] = sub code */
    if (obj_cmd_unused(id)) { stub_once("object cmd", id, c); return ELM_OK; }
    return ELM_UNKNOWN;
}

static ElmResult obj_elm(Obj *o, const int *rest, int n, ElmCall *c)
{
    if (!o) { elm_push_default(c); return ELM_OK; }
    while (n >= 3 && rest[0] == 93 && rest[1] == -1) {   /* obj.93.-1.<i>: child objects, walked without recursion */
        int i = rest[2];
        if (i < 0 || i >= o->nchild) {
            log_msg(LOG_WARN, "gfx: object index %d out of range (%d)", i, o->nchild);
            elm_push_default(c);
            return ELM_OK;
        }
        o = &o->child[i];
        rest += 3;
        n -= 3;
    }
    if (n == 0) { c->ret_obj = o; return ELM_OK; }   /* chain ends at the object (VM §4.1) */
    if (n > 1) return obj_sub_elm(o, rest[0], rest + 1, n - 1, c);
    if (c->op != OP_COMMAND && obj_prop_elm(o, rest[0], c)) return ELM_OK;
    return obj_cmd_elm(o, rest, c);
}

/* S.2.<sub>: the fixed object list of a stage (#OBJECT.CNT). */
static ElmResult stage_objects_elm(Obj *list, int cnt, const int *rest, int n, ElmCall *c)
{
    if (n >= 2 && rest[0] == -1) {
        int i = rest[1];
        if (i < 0 || i >= cnt) {
            log_msg(LOG_WARN, "gfx: object index %d out of range (%d)", i, cnt);
            elm_push_default(c);
            return ELM_OK;
        }
        return obj_elm(&list[i], rest + 2, n - 2, c);
    }
    if (n >= 1 && rest[0] == 3) { vm_push_int(cnt); return ELM_OK; }                 /* get_size */
    if (n >= 1 && (rest[0] == 4 || rest[0] == 1)) return ELM_OK;                       /* resize: fixed count */
    return ELM_UNKNOWN;
}

/* ------------------------------------------------------------ effects (FUN_00658180) and screen (FUN_00657950) */

static ElmResult tone_prop(IntEvent *e, ElmCall *c)
{
    if (c->op == OP_ASSIGN) { e->value = e->cur = px_clamp255(rhs_int(c)); }   /* FUN_006242d0..6244b0 clamp */
    else vm_push_int(e->value);
    return ELM_OK;
}

static ElmResult int_prop(int *v, ElmCall *c)
{
    if (c->op == OP_ASSIGN) *v = rhs_int(c); else vm_push_int(*v);
    return ELM_OK;
}

static IntEvent *eff_xyz(Effect *e, int k) { return k == 0 ? &e->x : k == 1 ? &e->y : &e->z; }

static ElmResult effect_elm(Effect *e, const int *rest, int n, ElmCall *c)
{
    if (!e) { elm_push_default(c); return ELM_OK; }
    if (n == 0) { c->ret_obj = e; return ELM_OK; }
    int id = rest[0];
    if (id >= ELM_EFFECT_X_EVE && id <= ELM_EFFECT_COLOR_ADD_B_EVE) {   /* x/y/z_eve, then the tone events */
        IntEvent *ev = id <= ELM_EFFECT_Z_EVE ? eff_xyz(e, id - ELM_EFFECT_X_EVE) : &e->tone[id - ELM_EFFECT_MONO_EVE];
        return ev_elm(ev, rest + 1, n - 1, c, id >= ELM_EFFECT_MONO_EVE);
    }
    if (n > 1) return ELM_UNKNOWN;
    if (id >= 0 && id <= 2) {
        IntEvent *ev = eff_xyz(e, id);
        if (c->op == OP_ASSIGN)
            ev->value = ev->cur = rhs_int(c);
        else
            vm_push_int(ev->value);
        return ELM_OK;
    }
    if (id >= ELM_EFFECT_MONO && id <= ELM_EFFECT_COLOR_ADD_B) return tone_prop(&e->tone[id - ELM_EFFECT_MONO], c);
    switch (id) {
    case ELM_EFFECT_BEGIN_ORDER: return int_prop(&e->begin_order, c);
    case ELM_EFFECT_END_ORDER: return int_prop(&e->end_order, c);
    case ELM_EFFECT_BEGIN_LAYER: return int_prop(&e->begin_layer, c);
    case ELM_EFFECT_END_LAYER: return int_prop(&e->end_layer, c);
    case ELM_EFFECT_INIT: gfx_effect_init(e); return ELM_OK;
    case ELM_EFFECT_WIPE_COPY:
    case ELM_EFFECT_WIPE_ERASE: {
        bool *b = id == ELM_EFFECT_WIPE_COPY ? &e->wipe_copy : &e->wipe_erase;
        if (c->op == OP_ASSIGN) *b = rhs_int(c) != 0; else vm_push_int(*b);
        return ELM_OK;
    }
    default: return ELM_UNKNOWN;
    }
}

static ElmResult screen_elm(const int *rest, int n, ElmCall *c)
{
    if (n == 0) { elm_push_default(c); return ELM_OK; }
    Effect *s = gfx_screen_tone();
    int id = rest[0];
    /* screen tone props 0,2..8,19..21 -> Tone order; events 11..18, 22..24 */
    static const signed char prop_tone[32] = {[0] = TONE_MONO + 1,   [2] = TONE_REVERSE + 1, [3] = TONE_BRIGHT + 1,
                                              [4] = TONE_DARK + 1,   [5] = TONE_R + 1,       [6] = TONE_G + 1,
                                              [7] = TONE_B + 1,      [8] = TONE_RATE + 1,    [19] = TONE_ADD_R + 1,
                                              [20] = TONE_ADD_G + 1, [21] = TONE_ADD_B + 1};
    static const signed char ev_tone[32] = {[11] = TONE_MONO + 1,  [12] = TONE_REVERSE + 1, [13] = TONE_BRIGHT + 1,
                                            [14] = TONE_DARK + 1,  [15] = TONE_R + 1,       [16] = TONE_G + 1,
                                            [17] = TONE_B + 1,     [18] = TONE_RATE + 1,    [22] = TONE_ADD_R + 1,
                                            [23] = TONE_ADD_G + 1, [24] = TONE_ADD_B + 1};
    switch (id) {
    case ELM_SCREEN_SHAKE: /* shake(n): FUN_005c74b0, blocks via proc 0x24 (key_skip 0) */
        if (gfx_shake_start(elm_arg_raw_int(c, 0, -1))) {
            proc_push_wait(PROC_SHAKE_WAIT, 0, false, false);
        }
        return ELM_OK;
    case ELM_SCREEN_EFFECT: /* effect list */
        if (n >= 3 && rest[1] == -1) {
            Effect *e = gfx_effect(rest[2]);
            if (!e) log_msg(LOG_WARN, "gfx: effect index %d out of range", rest[2]);
            return effect_elm(e, rest + 3, n - 3, c);
        }
        if (n >= 2 && rest[1] == 2) { vm_push_int(GFX_EFFECT_CNT); return ELM_OK; }
        if (n >= 2 && rest[1] == 1) return ELM_OK;   /* resize: fixed #EFFECT.CNT */
        return ELM_UNKNOWN;
    case ELM_SCREEN_INIT: gfx_effect_init(s); return ELM_OK;
    case ELM_SCREEN_QUAKE: stub_once("screen.quake", id, c); return ELM_OK;
    case ELM_SCREEN_X:
    case ELM_SCREEN_Y:
    case ELM_SCREEN_Z:
        if (n == 1) {
            IntEvent *ev = id == ELM_SCREEN_X ? &s->x : id == ELM_SCREEN_Y ? &s->y : &s->z;
            if (c->op == OP_ASSIGN)
                ev->value = ev->cur = rhs_int(c);
            else
                vm_push_int(ev->value);
            return ELM_OK;
        }
        return ELM_UNKNOWN;
    case ELM_SCREEN_X_EVE:
    case ELM_SCREEN_Y_EVE:
    case ELM_SCREEN_Z_EVE:
        return ev_elm(id == ELM_SCREEN_X_EVE ? &s->x : id == ELM_SCREEN_Y_EVE ? &s->y : &s->z, rest + 1, n - 1, c, 0);
    default:
        if (id >= 0 && id < 32 && prop_tone[id] && n == 1) return tone_prop(&s->tone[prop_tone[id] - 1], c);
        if (id >= 0 && id < 32 && ev_tone[id]) return ev_elm(&s->tone[ev_tone[id] - 1], rest + 1, n - 1, c, 1);
        return ELM_UNKNOWN;
    }
}

/* ------------------------------------------------------------ stage (FUN_00667bb0) */

static ElmResult stage_elm(int stage, const int *rest, int n, ElmCall *c)
{
    if (n == 0) { elm_push_default(c); return ELM_OK; }
    switch (rest[0]) {
    case ELM_STAGE_OBJECT: {
        Obj *list = gfx_obj(stage, 0);
        if (n >= 2 && (rest[1] == 4)) return ELM_OK;   /* objlist resize: fixed #OBJECT.CNT */
        return stage_objects_elm(list, GFX_OBJ_CNT, rest + 1, n - 1, c);
    }
    case ELM_STAGE_OBJBTNGROUP: return btn_elm_group(stage, rest + 1, n - 1, c); /* WP6 (ENGINE_SPEC §1.4.4) */
    case ELM_STAGE_MWND:                                                         /* S.3.-1.n.<sub>: mwnd list (WP4) */
        if (n >= 3 && rest[1] == -1 && g_stage_mwnd) return g_stage_mwnd(stage, rest + 2, n - 2, c);
        return ELM_UNKNOWN;
    case ELM_STAGE_CREATE_OBJECT:
    case ELM_STAGE_CREATE_MWND:
    case ELM_STAGE_EFFECT:
    case ELM_STAGE_BTNSELITEM:
    case ELM_STAGE_QUAKE:
    case ELM_STAGE_WORLD:
        stub_once("stage", rest[0], c);   /* create_object/mwnd, effect list, btnselitem, quake, world */
        return ELM_OK;
    default: return ELM_UNKNOWN;           /* 3 = mwnd list: not graphics */
    }
}

/* ------------------------------------------------------------ wipe (FUN_0066d380) */

static void read_opts(const Arg *a, int *opt)
{
    if (!a) return;
    if (a->sub) { for (int k = 0; k < a->sub->n && k < 8; k++) opt[k] = a->sub->v[k].i; }
    else opt[0] = a->i;
}

static void wipe_cmd(ElmCall *c, bool all)
{
    WipeArgs a;
    wipe_args_default(&a);
    if (all) a.end_order = INT_MAX;   /* global 23 */
    if (c->npos >= 1) a.type = elm_arg_raw_int(c, 0, 0);
    if (c->npos >= 2) a.time = elm_arg_raw_int(c, 1, 500);
    if (c->npos >= 3) a.speed = elm_arg_raw_int(c, 2, 0);
    if (c->arg && c->npos >= 4) read_opts(&c->arg[3], a.opt);
    for (int k = c->npos; c->arg && k < c->nall; k++) {
        const Arg *g = &c->arg[k];
        switch (g->id) {
        case 0: a.type = g->i; break;
        case 1: a.time = g->i; break;
        case 2: a.speed = g->i; break;
        case 3: read_opts(g, a.opt); break;
        case 4: a.begin_order = g->i; break;
        case 5: a.end_order = g->i; break;
        case 6: a.begin_layer = g->i; break;
        case 7: a.end_layer = g->i; break;
        case 8: a.wait = g->i != 0; break;
        case 9: a.key = g->i; break;
        case 11: a.start_time = g->i; break;
        default: break;
        }
    }
    gfx_wipe_start(&a);
}

/* ------------------------------------------------------------ global entry */

static ElmResult global_dispatch(int root, const int *rest, int nrest, ElmCall *c)
{
    switch (root) {
    case ELM_ROOT_WIPE: wipe_cmd(c, false); return ELM_OK;
    case ELM_ROOT_WIPE_ALL: wipe_cmd(c, true); return ELM_OK;
    case ELM_ROOT_MASK_WIPE_ALL:
    case ELM_ROOT_MASK_WIPE: stub_once("mask_wipe", root, c); return ELM_OK;
    case ELM_ROOT_WIPE_END: gfx_wipe_finish(); return ELM_OK;
    case ELM_ROOT_BACK: return stage_elm(GFX_STAGE_BACK, rest, nrest, c);
    case ELM_ROOT_FRONT: return stage_elm(GFX_STAGE_FRONT, rest, nrest, c);
    case ELM_ROOT_NEXT: return stage_elm(GFX_STAGE_NEXT, rest, nrest, c);
    case ELM_ROOT_SCREEN: return screen_elm(rest, nrest, c);
    case ELM_ROOT_WAIT_WIPE: { /* wait_wipe(key_skip=): proc 0x23, result via the proc (TXT §3.9) */
        const Arg *k = elm_arg_named(c, 0);
        gfx_push_wipe_wait(k ? (k->i != 0) : -1, true);
        return ELM_OK;
    }
    case ELM_ROOT_CHECK_WIPE: vm_push_int(gfx_wipe_active()); return ELM_OK;
    default: return ELM_UNKNOWN;
    }
}

static ElmResult gfx_elm_global(int root, const int *rest, int nrest, ElmCall *c)
{
    ElmChain outer = g_call;
    g_call = (ElmChain){ root, rest };
    ElmResult r = global_dispatch(root, rest, nrest, c);
    g_call = outer;
    return r;
}

/* ------------------------------------------------------------ procs */

/* FUN_005a2830: 0 when finished or skipped, 1 on a click with key_skip; the wipe is ended either way */
static bool proc_wipe_wait(Proc *p)
{
    int ret = -1;
    if (proc_wait_skip(p) || !gfx_wipe_active()) ret = 0;
    else if (p->key_skip && proc_wait_click()) ret = 1;
    if (ret < 0) return false;
    bool push = p->push_result;
    gfx_wipe_finish();
    proc_pop();
    if (push) vm_push_int(ret);
    return true;
}

/* asm 0x5a2960: shake ended, skip, or click if key_skip */
static bool proc_shake_wait(Proc *p)
{
    if (gfx_shake_active() && !proc_wait_skip(p) && !(p->key_skip && proc_wait_click())) return false;
    gfx_shake_stop();
    proc_pop();
    return true;
}

/* resolve the IntEvent named by a stored chain (object / child / rep-event / effect / screen) */
static IntEvent *resolve_event(const int *ch, int n)
{
    int clamp;
    if (n >= 2 && ch[0] == 70) {
        Effect *s = gfx_screen_tone();
        if (ch[1] == 31 && n >= 5 && ch[2] == -1) {
            Effect *e = gfx_effect(ch[3]);
            int id = ch[4];
            if (!e || id < ELM_EFFECT_X_EVE || id > ELM_EFFECT_COLOR_ADD_B_EVE) return NULL;
            return id <= ELM_EFFECT_Z_EVE ? eff_xyz(e, id - ELM_EFFECT_X_EVE) : &e->tone[id - ELM_EFFECT_MONO_EVE];
        }
        static const signed char ev_tone[25] = { [11] = 1, [12] = 2, [13] = 3, [14] = 4, [15] = 5, [16] = 6, [17] = 7,
                                                 [18] = 8, [22] = 9, [23] = 10, [24] = 11 };
        if (ch[1] >= 28 && ch[1] <= 30) return ch[1] == 28 ? &s->x : ch[1] == 29 ? &s->y : &s->z;
        if (ch[1] >= 0 && ch[1] < 25 && ev_tone[ch[1]]) return &s->tone[ev_tone[ch[1]] - 1];
        return NULL;
    }
    /* object chain: longest prefix that resolves to an object, then the event id */
    for (int m = n - 1; m >= 4; m--) {
        Obj *o = gfx_resolve_obj(ch, m);
        if (!o) continue;
        int id = ch[m];
        if (m + 1 == n) return obj_event(o, id, &clamp);
        if (m + 3 == n && ch[m + 1] == -1) {
            int k = ch[m + 2];
            IntEvent *l = id == ELM_OBJECT_X_REP_EVE ? o->x_rep
                          : id == ELM_OBJECT_Y_REP_EVE ? o->y_rep
                          : id == ELM_OBJECT_Z_REP_EVE ? o->z_rep
                          : id == ELM_OBJECT_TR_REP_EVE ? o->tr_rep
                                      : NULL;
            int cnt = id == ELM_OBJECT_X_REP_EVE ? o->nx_rep
                      : id == ELM_OBJECT_Y_REP_EVE ? o->ny_rep
                      : id == ELM_OBJECT_Z_REP_EVE ? o->nz_rep
                      : id == ELM_OBJECT_TR_REP_EVE ? o->ntr_rep
                                  : 0;
            return (l && k >= 0 && k < cnt) ? &l[k] : NULL;
        }
        return NULL;
    }
    return NULL;
}

static bool proc_event_wait(Proc *p)
{
    IntEvent *e = resolve_event(p->chain, p->nchain);
    int ret = -1;
    if (!e || !ievent_active(e)) ret = 0;
    else if (proc_wait_skip(p)) { ievent_end(e); ret = 0; }
    else if (p->key_skip && proc_wait_click()) { ievent_end(e); ret = 1; }
    if (ret < 0) return false;
    bool push = p->push_result;
    proc_pop();
    if (push) vm_push_int(ret);
    return true;
}

void gfx_elm_init(void)
{
    static const int ROOTS[] = {
        ELM_ROOT_WIPE, ELM_ROOT_WIPE_ALL, ELM_ROOT_WIPE_END, ELM_ROOT_BACK, ELM_ROOT_FRONT, ELM_ROOT_STAGE,
        ELM_ROOT_MASK_WIPE_ALL, ELM_ROOT_MASK_WIPE, ELM_ROOT_SCREEN, ELM_ROOT_NEXT, ELM_ROOT_CAPTURE,
        ELM_ROOT_CAPTURE_FREE, ELM_ROOT_FRAME, ELM_ROOT_WAIT_WIPE, ELM_ROOT_CHECK_WIPE, ELM_ROOT_G00BUF,
        ELM_ROOT_CAPTURE_FOR_OBJECT, ELM_ROOT_MASK, ELM_ROOT_CAPTURE_FOR_OBJECT_FREE, ELM_ROOT_CAPTURE_FOR_LOCAL_SAVE,
        ELM_ROOT_CAPTURE_FROM_FILE,
    };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), gfx_elm_global);
    proc_register(PROC_WIPE_WAIT, proc_wipe_wait);
    proc_register(PROC_SHAKE_WAIT, proc_shake_wait);
    proc_register(PROC_EVENT_WAIT, proc_event_wait);
}
