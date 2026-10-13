/* gfx/gfx_obj.c — object model: 3 stages x 256 objects, reset/copy/create, children, rep lists (GFX §2–3).
 *
 * Ownership: the stages are a fixed static table. What an object owns — its file name (WStr), its image key, its
 * rep lists and its child array — is on the tracked heap and is released by obj_init, so gfx_objects_reset frees
 * every object byte. Child arrays and rep lists are sized by scripts and saves, so they are MEM_OBJ blocks taken
 * with the try allocators: past the MEM_OBJ budget (core/mem.h) a resize or copy is refused with a warning (and a
 * save that needs it fails to load) instead of exhausting memory; a save can otherwise ask for about 90 times its
 * size in objects. Trees are at most GFX_OBJ_DEPTH_MAX levels deep (root = depth 0):
 * resizing refuses children below that depth and copies are pruned to it, which caps every recursion over an
 * object tree. Child arrays move when resized; set_child_links re-points the parent links of the whole subtree. */
#include "gfx/gfx.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/cgtable.h"
#include "gfx/render.h"
#include <string.h>

static Obj g_obj[GFX_STAGE_CNT][GFX_OBJ_CNT];
static bool g_obj_ready;
static void (*g_cg_hook)(int flag_no);

void gfx_set_cg_hook(void (*fn)(int flag_no)) { g_cg_hook = fn; }

/* ------------------------------------------------------------ owned memory */

static void rep_free(IntEvent **list, int *n)
{
    mem_free(*list);
    *list = NULL;
    *n = 0;
}

static void obj_drop_image(Obj *o)
{
    o->type = OBJ_NONE;
    ws_free(&o->file);
    mem_free(o->img_key);
    o->img_key = NULL;
    o->rect_rgb = 0;
    o->rect_x = o->rect_y = o->rect_w = o->rect_h = 0;
}


/* FUN_005eeb30 defaults (GFX §3.1): scale 1000, tr 255 (ENGINE_SPEC §1.3 #11), world -1, the rest 0 */
static void obj_defaults(Obj *o)
{
    o->disp = false;
    o->layer = o->order = 0;
    IntEvent *ev[] = GFX_OBJ_EVENTS(o);
    for (size_t k = 0; k < COUNT_OF(ev); k++) ievent_init(ev[k], 0);
    ievent_init(&o->scale_x, 1000); ievent_init(&o->scale_y, 1000); ievent_init(&o->scale_z, 1000);
    ievent_init(&o->tr, 255);
    o->clip_use = o->src_clip_use = false;
    for (int k = 0; k < TONE_CNT; k++) ievent_init(&o->tone[k], 0);
    rep_free(&o->x_rep, &o->nx_rep); rep_free(&o->y_rep, &o->ny_rep);
    rep_free(&o->z_rep, &o->nz_rep); rep_free(&o->tr_rep, &o->ntr_rep);
    o->world = -1;
    o->blend = o->tonecurve = o->click_disable = o->light_no = o->fog_use = o->mask_no = 0;
    o->culling = o->alpha_test = o->alpha_blend = 0;
}

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void obj_init_at(Obj *o, int depth)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    for (int k = 0; k < o->nchild; k++) obj_init_at(&o->child[k], depth + 1);
    mem_free(o->child);
    o->child = NULL;
    o->nchild = 0;
    obj_drop_image(o);
    obj_defaults(o);
    o->wipe_copy = o->wipe_erase = false;
    o->is_button = false;
    o->btn_no = o->btn_group = o->btn_action = o->btn_se = -1;
    o->btn_state = 0;
}

void obj_init(Obj *o) { if (o) obj_init_at(o, 0); }   /* a subtree is never deeper than the cap */

void obj_free(Obj *o) { if (o) obj_drop_image(o); }
void obj_init_param(Obj *o) { if (o) obj_defaults(o); }

int obj_depth(const Obj *o)
{
    int d = 0;
    for (const Obj *p = o->parent; p && d <= GFX_OBJ_DEPTH_MAX; p = p->parent) d++;
    return d;
}

/* ------------------------------------------------------------ names */

void obj_set_image(Obj *o, const WStr *name)
{
    if (name != &o->file) ws_copy(&o->file, name);
    mem_free(o->img_key);
    Scratch sc = mem_scratch_begin(NULL);
    o->img_key = mem_strdup(ws_utf8_arena(sc.arena, &o->file, WS_UTF8_SHORT_MAX), MEM_GFX);
    mem_scratch_end(&sc);
    for (char *p = o->img_key; *p; p++) *p = str_ascii_upper(*p);
}

/* SYS §6.3: every "a|b" part of an image key (without its "?N" suffix) that is a CG unlocks its flag. */
static void cg_unlock(const char *key)
{
    if (!g_cg_hook || !key) return;
    Str rest = str_c(key), part;
    while (str_split(&rest, '|', &part)) {
        size_t q;
        if (str_find_char(part, '?', &q)) part.n = q;
        char name[256];
        if (!part.n || !str_copy(name, sizeof name, part)) continue;   /* longer than any CG name */
        int f = cgt_flag_no(name);
        if (f >= 0) g_cg_hook(f);
    }
}

bool obj_create_image(Obj *o, const WStr *name)
{
    if (!o) return false;
    obj_init(o);   /* FUN_005ee710(1): full reset */
    o->type = OBJ_IMAGE;
    obj_set_image(o, name);
    bool ok = img_exists(o->img_key);
    if (ok) cg_unlock(o->img_key);
    if (log_trace_on("GFX"))
        log_trace("GFX", "obj %d.%d create %s%s", o->stage, o->idx, o->img_key, ok ? "" : " (missing)");
    return ok;
}

void obj_create_rect(Obj *o, int x, int y, int w, int h, int r, int g, int b, int a)
{
    if (!o) return;
    obj_init(o);
    o->type = OBJ_RECT;
    o->rect_x = x; o->rect_y = y; o->rect_w = w; o->rect_h = h;
    o->rect_rgb =
        (uint32_t)(a & 255) << 24 | (uint32_t)(r & 255) << 16 | (uint32_t)(g & 255) << 8 | (uint32_t)(b & 255);
}

int obj_pat_count(const Obj *o)
{
    return (o && o->type == OBJ_IMAGE && o->img_key) ? img_pat_count(o->img_key) : 0;
}

/* ------------------------------------------------------------ trees */

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void set_child_links(Obj *o, int depth)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    for (int k = 0; k < o->nchild; k++) {
        Obj *c = &o->child[k];
        c->parent = o; c->stage = o->stage; c->idx = k;
        set_child_links(c, depth + 1);
    }
}

static void obj_budget_refused(const char *what, int n)
{
    static bool logged;
    log_once(&logged, LOG_WARN, "gfx: %s of %d refused: the object memory budget is exhausted", what, n);
}

/* A copy of a rep list; NULL (with *n = 0) when it is empty or the budget refuses it. */
static IntEvent *dup_events(const IntEvent *src, int *n)
{
    if (!src || *n <= 0) { *n = 0; return NULL; }
    IntEvent *d = mem_try_alloc_array((size_t)*n, sizeof *src, MEM_OBJ);
    if (!d) { obj_budget_refused("rep list copy", *n); *n = 0; return NULL; }
    memcpy(d, src, sizeof *src * (size_t)*n);
    return d;
}

/* Deep clone into raw memory (dst is overwritten without freeing); children below depth_max are dropped.
 * name_buf, when not NULL, is a WStr whose buffer the clone's file name takes over (obj_copy passes the target's
 * old one, so the stage copies of every wipe reuse buffers instead of allocating). */
/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void obj_clone(Obj *dst, const Obj *src, int depth, WStr *name_buf)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    *dst = *src;
    memset(&dst->file, 0, sizeof dst->file);
    if (name_buf) { dst->file = *name_buf; memset(name_buf, 0, sizeof *name_buf); }
    if (src->file.len > 0 || dst->file.p) ws_copy(&dst->file, &src->file);   /* empty stays unallocated */
    dst->img_key = src->img_key ? mem_strdup(src->img_key, MEM_GFX) : NULL;
    dst->x_rep = dup_events(src->x_rep, &dst->nx_rep);
    dst->y_rep = dup_events(src->y_rep, &dst->ny_rep);
    dst->z_rep = dup_events(src->z_rep, &dst->nz_rep);
    dst->tr_rep = dup_events(src->tr_rep, &dst->ntr_rep);
    dst->child = NULL;
    dst->nchild = 0;
    if (src->nchild <= 0) return;
    if (depth >= GFX_OBJ_DEPTH_MAX) {
        log_msg(LOG_WARN, "gfx: obj.copy: children below depth %d dropped", GFX_OBJ_DEPTH_MAX);
        return;
    }
    dst->child = mem_try_alloc_array((size_t)src->nchild, sizeof(Obj), MEM_OBJ);
    if (!dst->child) { obj_budget_refused("object copy with children", src->nchild); return; }
    dst->nchild = src->nchild;
    for (int k = 0; k < src->nchild; k++) obj_clone(&dst->child[k], &src->child[k], depth + 1, NULL);
}

void obj_copy(Obj *dst, const Obj *src)   /* FUN_005f3420(src, 0): everything incl. params */
{
    if (!dst || !src || dst == src) return;
    int depth = obj_depth(dst);
    Obj tmp;
    WStr name = dst->file;          /* dst's name buffer is reused (src != dst, so src->file is another buffer) */
    memset(&dst->file, 0, sizeof dst->file);
    obj_clone(&tmp, src, depth, &name);   /* first: src may live inside dst's subtree, which obj_init frees */
    int stage = dst->stage, idx = dst->idx;
    Obj *parent = dst->parent;
    obj_init(dst);
    ws_free(&dst->file);
    *dst = tmp;
    dst->stage = stage; dst->idx = idx; dst->parent = parent;
    set_child_links(dst, depth);
}

/* The array of an object's children grown to n (> o->nchild) entries; false (nothing changed) when refused. */
static bool children_grow(Obj *o, int n)
{
    Obj *c = o->child ? mem_try_realloc_array(o->child, (size_t)n, sizeof(Obj))
                      : mem_try_alloc_array((size_t)n, sizeof(Obj), MEM_OBJ);
    if (!c) { obj_budget_refused("object children", n); return false; }
    int old = o->nchild;
    o->child = c;
    o->nchild = n;
    memset(o->child + old, 0, sizeof(Obj) * (size_t)(n - old));
    for (int k = old; k < n; k++) { o->child[k].parent = o; obj_init(&o->child[k]); }
    return true;
}

bool obj_children_resize(Obj *o, int n)
{
    if (!o || n < 0 || n > GFX_OBJ_CHILD_MAX) return false;
    if (n == o->nchild) return true;
    if (n > 0 && obj_depth(o) >= GFX_OBJ_DEPTH_MAX) {
        log_msg(LOG_WARN, "gfx: object children deeper than %d refused", GFX_OBJ_DEPTH_MAX);
        return false;
    }
    if (n > o->nchild && !children_grow(o, n)) return false;
    for (int k = n; k < o->nchild; k++) obj_init(&o->child[k]);
    if (n == 0) { mem_free(o->child); o->child = NULL; }
    o->nchild = n;                             /* shrinking keeps the block: nothing can fail */
    set_child_links(o, obj_depth(o));
    return true;
}

bool obj_rep_resize(IntEvent **list, int *n, int newn)
{
    if (newn < 0 || newn > GFX_REP_MAX) return false;
    if (newn == *n) return true;
    if (newn == 0) { rep_free(list, n); return true; }
    if (newn > *n) {
        IntEvent *l = *list ? mem_try_realloc_array(*list, (size_t)newn, sizeof(IntEvent))
                            : mem_try_alloc_array((size_t)newn, sizeof(IntEvent), MEM_OBJ);
        if (!l) { obj_budget_refused("rep list", newn); return false; }
        *list = l;
        for (int k = *n; k < newn; k++) ievent_init(&(*list)[k], 0);
    }
    *n = newn;
    return true;
}

static void events_frame(IntEvent *l, int n, int32_t g, int32_t r)
{
    for (int k = 0; k < n; k++) ievent_frame(&l[k], g, r);
}

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void obj_frame_at(Obj *o, int32_t g, int32_t r, int depth)
{
    ASSERT(depth <= GFX_OBJ_DEPTH_MAX);
    IntEvent *ev[] = GFX_OBJ_EVENTS(o);
    for (size_t k = 0; k < COUNT_OF(ev); k++) ievent_frame(ev[k], g, r);
    events_frame(o->tone, TONE_CNT, g, r);
    events_frame(o->x_rep, o->nx_rep, g, r);
    events_frame(o->y_rep, o->ny_rep, g, r);
    events_frame(o->z_rep, o->nz_rep, g, r);
    events_frame(o->tr_rep, o->ntr_rep, g, r);
    for (int k = 0; k < o->nchild; k++) obj_frame_at(&o->child[k], g, r, depth + 1);
}

void obj_frame(Obj *o, int32_t g, int32_t r) { obj_frame_at(o, g, r, obj_depth(o)); }

/* ------------------------------------------------------------ stages */

static void stages_init(void)
{
    for (int s = 0; s < GFX_STAGE_CNT; s++)
        for (int i = 0; i < GFX_OBJ_CNT; i++) {
            Obj *o = &g_obj[s][i];
            if (!g_obj_ready) memset(o, 0, sizeof *o);
            obj_init(o);
            o->stage = s; o->idx = i; o->parent = NULL;
        }
    g_obj_ready = true;
}

void gfx_objects_reset(void) { stages_init(); }

Obj *gfx_obj(int stage, int idx)
{
    if (!g_obj_ready) stages_init();
    if (stage < 0 || stage >= GFX_STAGE_CNT || idx < 0 || idx >= GFX_OBJ_CNT) return NULL;
    return &g_obj[stage][idx];
}

static int stage_of_root(int root)
{ return root == 37 ? GFX_STAGE_BACK : root == 38 ? GFX_STAGE_FRONT : root == 73 ? GFX_STAGE_NEXT : -1; }

Obj *gfx_resolve_obj(const int *chain, int n)
{
    if (!chain || n < 4) return NULL;
    int stage = stage_of_root(chain[0]);
    if (stage < 0 || chain[1] != 2 || chain[2] != -1) return NULL;
    Obj *o = gfx_obj(stage, chain[3]);
    for (int k = 4; o && k < n; k += 3) {   /* (93, -1, k)* = child objects */
        if (k + 3 > n || chain[k] != 93 || chain[k + 1] != -1) return NULL;
        int c = chain[k + 2];
        o = (c >= 0 && c < o->nchild) ? &o->child[c] : NULL;
    }
    return o;
}

/* Accumulated parent state for an arbitrary object (children inherit position, tr, clip, disp). */
static void parent_ctx(const Obj *o, RenderCtx *ctx)
{
    render_ctx_root(ctx);
    const Obj *chain[GFX_OBJ_DEPTH_MAX + 1];
    int n = 0;
    for (const Obj *p = o->parent; p && n < (int)COUNT_OF(chain); p = p->parent) chain[n++] = p;
    for (int k = n - 1; k >= 0; k--) render_ctx_child(ctx, chain[k], ctx);
}

bool obj_screen_rect(const Obj *o, Rect *out)
{
    if (!o) return false;
    RenderCtx ctx;
    parent_ctx(o, &ctx);
    ObjDraw d;
    if (!render_obj_prepare(o, &ctx, &d) || !d.visible) return false;
    *out = d.dst;
    return true;
}

bool gfx_obj_hit(const Obj *o, int x, int y)
{
    Rect r;
    if (!o || !obj_screen_rect(o, &r)) return false;
    return x >= r.x0 && x < r.x1 && y >= r.y0 && y < r.y1;
}
