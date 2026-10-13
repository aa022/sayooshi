/* gfx/render.c — object composite (GFX §2.3, §3.4, §3.5; ENGINE_SPEC §2.9 steps 1-2).
 * Script values are summed with two's-complement wrap and every drawn rect goes through the clamped rect helpers
 * of canvas.c, so no object state can overflow. Sort orders are per-call scratch memory: no heap use per frame. */
#include "gfx/render.h"
#include "core/log.h"
#include "core/mem.h"
#include "gfx/button.h"
#include "gfx/pixel.h"
#include <stdlib.h>
#include <string.h>


static int rep_sum(const IntEvent *l, int n)
{
    int s = 0;
    for (int k = 0; k < n; k++) s = core_wrap_add_i32(s, l[k].cur);
    return s;
}

static int tr_mul(int a, int b) { return (a * b + 127) / 255; }   /* a, b in 0..255 */

void render_ctx_root(RenderCtx *ctx)
{
    ctx->px = ctx->py = 0;
    ctx->tr = 255;
    ctx->clip = rect_make(0, 0, SCREEN_W, SCREEN_H);
    ctx->disp = true;
    ctx->order = ctx->layer = 0;
}

/* FUN_005f77c0: position = x + center_rep_x + sum(x_rep) (GFX §3.4) */
static void obj_pos(const Obj *o, int *px, int *py)
{
    *px = core_wrap_add_i32(core_wrap_add_i32(o->x.cur, o->center_rep_x.cur), rep_sum(o->x_rep, o->nx_rep));
    *py = core_wrap_add_i32(core_wrap_add_i32(o->y.cur, o->center_rep_y.cur), rep_sum(o->y_rep, o->ny_rep));
}

static int obj_tr(const Obj *o) { return px_clamp255(core_wrap_add_i32(o->tr.cur, rep_sum(o->tr_rep, o->ntr_rep))); }

static Rect obj_clip(const Obj *o, const Rect *in)
{
    Rect r = *in;
    if (o->clip_use) {   /* screen space, right/bottom exclusive (GFX §3.4) */
        Rect c = { o->clip_l.cur, o->clip_t.cur, o->clip_r.cur, o->clip_b.cur };
        if (!rect_intersect(&r, &c)) r.x1 = r.x0;
    }
    return r;
}

/* out may alias parent_ctx (parent_ctx is copied first). */
void render_ctx_child(const RenderCtx *parent_ctx, const Obj *parent, RenderCtx *out)
{
    RenderCtx c = *parent_ctx;
    int x, y;
    obj_pos(parent, &x, &y);
    c.px = core_wrap_add_i32(c.px, x);
    c.py = core_wrap_add_i32(c.py, y);
    c.tr = tr_mul(c.tr, obj_tr(parent));
    c.clip = obj_clip(parent, &parent_ctx->clip);
    c.disp = parent_ctx->disp && parent->disp;
    c.order = core_wrap_add_i32(c.order, parent->order);
    c.layer = core_wrap_add_i32(c.layer, parent->layer);
    *out = c;
}

static Tone obj_tone(const Obj *o, int bright, int dark)
{
    const IntEvent *t = o->tone;
    return (Tone){px_clamp255(t[TONE_MONO].cur),
                  px_clamp255(t[TONE_REVERSE].cur),
                  px_clamp255(core_wrap_add_i32(t[TONE_BRIGHT].cur, bright)),
                  px_clamp255(core_wrap_add_i32(t[TONE_DARK].cur, dark)),
                  px_clamp255(t[TONE_R].cur),
                  px_clamp255(t[TONE_G].cur),
                  px_clamp255(t[TONE_B].cur),
                  px_clamp255(t[TONE_RATE].cur),
                  px_clamp255(t[TONE_ADD_R].cur),
                  px_clamp255(t[TONE_ADD_G].cur),
                  px_clamp255(t[TONE_ADD_B].cur)};
}

static void rotation_unsupported(const Obj *o)
{
    static bool once;
    if (o->rotate_z.cur % 3600 && !once) {
        once = true;
        log_msg(LOG_DEBUG, "gfx: object rotation not implemented (GFX §9 stub)");
    }
}

/* Image geometry: FUN_006785c0 local rect relative to the centre (cx, cy); src_clip is centre-relative.
 * Fills d->img/src/dx/dy and the float screen rect; false when nothing is drawn. */
static bool prepare_image(const Obj *o, ObjDraw *d, int px, int py, int cx, int cy, int pat_add, float f[4])
{
    d->img = o->img_key ? img_get(o->img_key, core_wrap_add_i32(o->patno.cur, pat_add)) : NULL;
    if (!d->img) return false;
    cx = core_wrap_add_i32(cx, d->img->cx);
    cy = core_wrap_add_i32(cy, d->img->cy);
    int64_t L = -(int64_t)cx, T = -(int64_t)cy, R = (int64_t)d->img->w - cx, B = (int64_t)d->img->h - cy;
    if (o->src_clip_use) {
        if (L < o->src_l.cur) L = o->src_l.cur;
        if (T < o->src_t.cur) T = o->src_t.cur;
        if (R > o->src_r.cur) R = o->src_r.cur;
        if (B > o->src_b.cur) B = o->src_b.cur;
    }
    if (L >= R || T >= B) return false;
    d->src = rect_make((int)(L + cx), (int)(T + cy), (int)(R + cx), (int)(B + cy));   /* inside the image */
    f[0] = (float)px + (float)L * d->sx; f[1] = (float)py + (float)T * d->sy;
    f[2] = (float)px + (float)R * d->sx; f[3] = (float)py + (float)B * d->sy;
    return true;
}

static bool prepare_rect(const Obj *o, ObjDraw *d, int px, int py, int cx, int cy, float f[4])
{
    if (o->rect_w <= 0 || o->rect_h <= 0) return false;
    d->rect_col = o->rect_rgb;
    f[0] = (float)px + (float)((int64_t)o->rect_x - cx) * d->sx;
    f[1] = (float)py + (float)((int64_t)o->rect_y - cy) * d->sy;
    f[2] = f[0] + (float)o->rect_w * d->sx;
    f[3] = f[1] + (float)o->rect_h * d->sy;
    return true;
}

bool render_obj_prepare(const Obj *o, const RenderCtx *ctx, ObjDraw *d)
{
    memset(d, 0, sizeof *d);
    d->type = o->type;
    if (o->type != OBJ_IMAGE && o->type != OBJ_RECT) return true;   /* other object types are unused (GFX §3.3) */
    int pat_add = 0, bdx = 0, bdy = 0, btr = 255, bright = 0, dark = 0;
    if (o->is_button) btn_visual(o, &pat_add, &bdx, &bdy, &btr, &bright, &dark);   /* §1.4.4 */
    int px, py;
    obj_pos(o, &px, &py);
    px = core_wrap_add_i32(px, core_wrap_add_i32(ctx->px, bdx));
    py = core_wrap_add_i32(py, core_wrap_add_i32(ctx->py, bdy));
    d->tr = tr_mul(tr_mul(ctx->tr, obj_tr(o)), px_clamp255(btr));
    d->clip = obj_clip(o, &ctx->clip);
    d->tone = obj_tone(o, bright, dark);
    d->sx = (float)o->scale_x.cur / 1000.0f;
    d->sy = (float)o->scale_y.cur / 1000.0f;
    int cx = core_wrap_add_i32(o->center_x.cur, o->center_rep_x.cur),
        cy = core_wrap_add_i32(o->center_y.cur, o->center_rep_y.cur);
    rotation_unsupported(o);
    float f[4];
    bool any =
        o->type == OBJ_IMAGE ? prepare_image(o, d, px, py, cx, cy, pat_add, f) : prepare_rect(o, d, px, py, cx, cy, f);
    if (!any || d->sx <= 0 || d->sy <= 0) return true;
    d->dx = f[0];
    d->dy = f[1];
    d->dst = rect_cover(f[0], f[1], f[2], f[3]);
    d->visible = rect_intersect(&d->dst, &d->clip) && ctx->disp && o->disp && d->tr > 0;
    return true;
}

static void draw_prepared(Canvas *dst, const ObjDraw *d)
{
    if (!d->visible) return;
    if (d->type == OBJ_IMAGE) {
        canvas_blit_ex(dst, d->img, d->src, d->dx, d->dy, d->sx, d->sy, d->tr, &d->tone, &d->clip);
    } else if (d->type == OBJ_RECT) {
        uint32_t c = d->rect_col;
        if (!tone_is_identity(&d->tone)) {   /* tone the colour once */
            uint32_t p = tone_apply_px(0xff000000u | (c & 0xffffffu), &d->tone);
            c = (c & 0xff000000u) | (p & 0xffffffu);
        }
        canvas_fill(dst, d->dst, c, d->tr);
    }
}

/* ------------------------------------------------------------ sort order */

typedef struct SortKey { int order, layer, idx; } SortKey;

static int cmp_int(int a, int b) { return (a > b) - (a < b); }

static int key_cmp(const void *a, const void *b)
{
    const SortKey *x = a, *y = b;
    int c = cmp_int(x->order, y->order);
    if (!c) c = cmp_int(x->layer, y->layer);
    return c ? c : cmp_int(x->idx, y->idx);   /* ties: lower index first (ENGINE_SPEC §1.6) */
}

static bool obj_drawn(const Obj *o) { return o->type != OBJ_NONE || o->nchild > 0; }

int render_sorted(const Obj *objs, int n, int *out)
{
    if (n <= 0) return 0;
    Scratch s = mem_scratch_begin(NULL);
    SortKey *k = ARENA_NEW_ARRAY(s.arena, SortKey, (size_t)n);
    int m = 0;
    for (int i = 0; i < n; i++)
        if (obj_drawn(&objs[i])) k[m++] = (SortKey){ objs[i].order, objs[i].layer, i };
    qsort(k, (size_t)m, sizeof *k, key_cmp);
    for (int i = 0; i < m; i++) out[i] = k[i].idx;
    mem_scratch_end(&s);
    return m;
}

static int range_cmp(int o1, int l1, int o2, int l2)
{
    int c = cmp_int(o1, o2);
    return c ? c : cmp_int(l1, l2);
}

bool render_range_contains(const WipeRange *r, int order, int layer)
{
    return range_cmp(r->begin_order, r->begin_layer, order, layer) <= 0 &&
           range_cmp(order, layer, r->end_order, r->end_layer) <= 0;
}

static bool range_selects(const WipeRange *r, int mode, const Obj *o)
{
    if (mode == RANGE_ALL || !r) return true;
    bool in = render_range_contains(r, o->order, o->layer);
    bool below = range_cmp(o->order, o->layer, r->begin_order, r->begin_layer) < 0;
    switch (mode) {
    case RANGE_IN: return in;
    case RANGE_BELOW: return below;
    case RANGE_ABOVE: return !in && !below;
    default: return true;
    }
}

/* ------------------------------------------------------------ drawing */

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void render_obj_at(Canvas *dst, const Obj *o, const RenderCtx *ctx, int depth)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    ObjDraw d;
    (void)render_obj_prepare(o, ctx, &d);   /* always true: d says whether anything is drawn */
    draw_prepared(dst, &d);
    if (o->nchild <= 0) return;
    RenderCtx cc;   /* children follow their parent, sorted among themselves */
    render_ctx_child(ctx, o, &cc);
    if (!cc.disp) return;
    Scratch s = mem_scratch_begin(NULL);
    int *idx = ARENA_NEW_ARRAY(s.arena, int, (size_t)o->nchild);
    int m = render_sorted(o->child, o->nchild, idx);
    for (int i = 0; i < m; i++) render_obj_at(dst, &o->child[idx[i]], &cc, depth + 1);
    mem_scratch_end(&s);
}

void render_stage_range(Canvas *dst, int stage, const Rect *clip, const WipeRange *r, int mode)
{
    const Obj *objs = gfx_obj(stage, 0);
    if (!objs) return;
    int idx[GFX_OBJ_CNT];
    int m = render_sorted(objs, GFX_OBJ_CNT, idx);
    RenderCtx ctx;
    render_ctx_root(&ctx);
    if (clip) (void)rect_intersect(&ctx.clip, clip);   /* an empty clip draws nothing */
    for (int i = 0; i < m; i++)
        if (range_selects(r, mode, &objs[idx[i]])) render_obj_at(dst, &objs[idx[i]], &ctx, 0);
}

void render_stage(Canvas *dst, int stage, const Rect *clip) { render_stage_range(dst, stage, clip, NULL, RANGE_ALL); }
