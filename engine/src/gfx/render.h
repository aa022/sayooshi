/* gfx/render.h — internal API of the gfx module: object composite, wipe compositing and state, image cache
 * maintenance, object tree helpers (ENGINE_SPEC §2.9, GFX §2.3, §3.4, §4.3). Only gfx/ (and the legacy WP3 test
 * driver) include it. */
#ifndef GFX_RENDER_H
#define GFX_RENDER_H
#include "core/base.h"
#include "core/str.h"
#include "gfx/canvas.h"
#include "gfx/gfx.h"

/* ---- object trees (gfx_obj.c) ---- */
enum {
    GFX_OBJ_DEPTH_MAX = 8,      /* deepest child level (root = 0); also the save format's limit */
    GFX_OBJ_CHILD_MAX = 4096,   /* children per object */
    GFX_REP_MAX = 4096,         /* entries per rep list */
    GFX_OBJ_EVENT_CNT = 25,     /* the IntEvent properties of GFX_OBJ_EVENTS */
};
/* The IntEvent properties of an object in canonical (save format) order, as an array initialiser. */
#define GFX_OBJ_EVENTS(o) { &(o)->patno, &(o)->x, &(o)->y, &(o)->z, &(o)->center_x, &(o)->center_y, &(o)->center_z, \
    &(o)->center_rep_x, &(o)->center_rep_y, &(o)->center_rep_z, &(o)->scale_x, &(o)->scale_y, &(o)->scale_z, \
    &(o)->rotate_x, &(o)->rotate_y, &(o)->rotate_z, &(o)->clip_l, &(o)->clip_t, &(o)->clip_r, &(o)->clip_b, \
    &(o)->src_l, &(o)->src_t, &(o)->src_r, &(o)->src_b, &(o)->tr }

int  obj_depth(const Obj *o);                         /* 0 for a stage object */

/* ---- button elements (button_elm.c; gfx_elm.c forwards stage.6 and the object button sub-commands) ---- */
ElmResult btn_elm_group(int stage, const int *rest, int nrest, ElmCall *c);   /* stage.6 objbtngroup */
ElmResult btn_elm_obj(Obj *o, const int *rest, int nrest, ElmCall *c);        /* obj.42 set_button and friends */
void obj_set_image(Obj *o, const WStr *name);         /* file = name (may be &o->file), img_key = its upper case */

/* ---- rects (canvas.c): clamped to +-2^24, so intersections with any canvas stay exact (rect_at: canvas.h) ---- */
Rect rect_cover(float x0, float y0, float x1, float y1);   /* pixels whose centres lie in [x0,x1) x [y0,y1) */

/* ---- image cache (img_cache.c) ---- */
const Image *img_get_ws(const WStr *name, int patno);   /* img_get for a script string */
void img_cache_trim(void);                               /* once per frame (gfx_frame): evict down to the budget */
void img_cache_shutdown(void);                           /* frees every image and the table */

/* ---- composite (render.c) ---- */
/* Inherited state for children (GFX §2.3: position/order/layer added, disp and-ed; tr multiplied and clip
 * intersected [I]). */
typedef struct RenderCtx { int px, py; int tr; Rect clip; bool disp; int order, layer; } RenderCtx;

/* Everything needed to draw one object (also used for hit tests). */
typedef struct ObjDraw {
    bool visible; /* something would be drawn */
    int type;
    const Image *img;
    Rect src;
    float dx, dy, sx, sy; /* image: src rect placed at (dx,dy) scaled */
    uint32_t rect_col;    /* type 1: straight ARGB */
    Rect clip;            /* screen-space scissor */
    Rect dst;             /* drawn screen rect (after clip) */
    int tr;
    Tone tone;
} ObjDraw;

/* (order, layer) wipe range, compared lexicographically (GFX §2.1). */
typedef struct WipeRange { int begin_order, begin_layer, end_order, end_layer; } WipeRange;
enum { RANGE_ALL, RANGE_IN, RANGE_BELOW, RANGE_ABOVE };

void render_ctx_root(RenderCtx *ctx);
void render_ctx_child(const RenderCtx *parent_ctx, const Obj *parent, RenderCtx *out);
bool render_obj_prepare(const Obj *o, const RenderCtx *ctx, ObjDraw *out);   /* always true; see out->visible */
int render_sorted(const Obj *objs, int n, int *out_idx); /* (order, layer, index) ascending; skips empty; out_idx[n] */
bool render_range_contains(const WipeRange *r, int order, int layer);

void render_stage(Canvas *dst, int stage, const Rect *clip);          /* sorted by (order, layer, index) */
void render_stage_range(Canvas *dst, int stage, const Rect *clip, const WipeRange *r, int mode);

/* ---- wipe compositing (wipe.c) ---- */
/* Composite OLD/NEW over dst by wipe type (GFX §4.3). OLD and NEW are premultiplied canvases (transparent where
 * nothing was drawn); dst already holds the out-of-range objects below. */
void wipe_composite(Canvas *dst, const Canvas *old_c, const Canvas *new_c, int type,
                    int32_t progress /*0..65536*/, const int *opts, int nopts);
uint8_t wipe_mask_value(int type, const int *opts, int nopts, int x, int y, int w, int h);  /* 100/101/102 mask m */
int  wipe_mask_alpha256(int m, int progress, int soft_opt);   /* per-pixel alpha 0..256 (shader 0x6b2604) */

/* ---- wipe state (gfx_wipe.c) ---- */
void gfx_wipe_reset(void);                       /* no wipe running (restart / load) */
void gfx_wipe_frame(int32_t game_dt);            /* progress; ends the wipe when the time is up */
void gfx_wipe_draw(Canvas *t);                   /* the front stage, through the running wipe if any */
int  gfx_wipe_progress(void);                    /* 0..65536 */
int  wipe_calc_progress(int elapsed, int time, int speed);   /* FUN_00558320 curves */

/* ---- frame state (gfx.c) ---- */
const Canvas *gfx_last_fb(void);                 /* framebuffer of the last gfx_render (owned by the caller) */
void gfx_shake_offset(int *dx, int *dy);         /* current shake step (GFX §5) */

#endif
