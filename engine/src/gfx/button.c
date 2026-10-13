/* gfx/button.c — object buttons, object button groups, proc 0x10 (ENGINE_SPEC §1.4.4, WP6).
 * Exe references: group element FUN_00667e80, reset FUN_005b94b0, decide FUN_005b9690, per-object button
 * update FUN_005fb910, proc 0x10 FUN_005a3080 (SPEC_ELEMENTS_SYS). */
#include "gfx/button.h"
#include "core/log.h"
#include "core/str.h"
#include "plat/input.h"
#include "vm/vm.h"
#include "fmt/gameexe.h"
#include "gfx/render.h"
#include "sav/sav_stream.h"
#include <string.h>

enum { MAX_BTN = 512, STAGE_FRONT = 1, BTN_SOURCE_MAX = 4096 /* test object sources */ };

static BtnGroup g_grp[BTN_STAGES][BTNGROUP_CNT];
static bool g_inited;
static const Obj *g_press;            /* button that got the mouse-L press; NULL = none */
static BtnObjSource g_src;
static BtnHitFn g_hit;
static BtnSelDone g_done;

/* ---- #BUTTON.ACTION table: (pat_add, dx, dy, tr, bright, dark) per state ---------------------------- */
enum { NACTION = 16 };
static int g_act[NACTION][5][6];
static bool g_act_loaded;
/* Gameexe.ini values of actions 000-009 (used when Gameexe is not loaded, e.g. in tests). */
static const int ACT_DEFAULT[10][5][6] = {
  {{0,0,0,255,0,0},{0,0,0,255,64,0},{0,1,1,255,64,0},{0,0,0,255,128,0},{0,0,0,255,0,128}},
  {{0,0,0,255,0,0},{1,0,0,255,0,0},{2,0,0,255,0,0},{3,0,0,255,0,0},{4,0,0,255,0,0}},
  {{0,0,0,255,0,64},{0,0,0,255,0,0},{0,1,1,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,128}},
  {{0,0,0,255,0,0},{0,-2,-2,128,64,0},{0,2,2,128,64,0},{0,0,0,64,128,0},{0,0,0,255,0,128}},
  {{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0}},
  {{0,0,0,180,0,0},{0,0,0,255,0,0},{0,1,1,255,0,0},{0,0,0,255,0,0},{0,0,0,96,0,0}},
  {{0,0,0,180,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,96,0,0}},
  {{0,0,0,0,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,255,0,0},{0,0,0,0,0,0}},
  {{0,0,0,255,0,0},{1,0,0,255,0,0},{2,0,0,255,0,0},{2,0,0,255,0,0},{0,0,0,128,0,0}},
  {{0,0,0,255,0,0},{10,0,0,255,0,0},{10,0,0,255,0,0},{10,0,0,255,0,0},{0,0,0,128,0,0}},
};

static void load_actions(void)
{
    static const char *const ST[5] = { "NORMAL", "HIT", "PUSH", "SELECT", "DISABLE" };
    for (int a = 0; a < NACTION; a++)
        for (int s = 0; s < 5; s++) {
            const int *def = a < 10 ? ACT_DEFAULT[a][s] : ACT_DEFAULT[4][s];
            char key[64];
            bool has = str_fmt(key, sizeof key, "BUTTON.ACTION.%03d.%s", a, ST[s]) && gx_has(key);   /* always fits */
            for (int k = 0; k < 6; k++) g_act[a][s][k] = has ? gx_int(key, k, def[k]) : def[k];
        }
    g_act_loaded = true;
}

void btn_visual(const Obj *o, int *pat_add, int *dx, int *dy, int *tr, int *bright, int *dark)
{
    int v[6] = { 0, 0, 0, 255, 0, 0 };
    if (o && o->is_button) {
        if (!g_act_loaded) load_actions();
        int a = o->btn_action, s = o->btn_state;
        if (a >= 0 && a < NACTION && s >= 0 && s < 5) memcpy(v, g_act[a][s], sizeof v);
    }
    if (pat_add) *pat_add = v[0];
    if (dx) *dx = v[1];
    if (dy) *dy = v[2];
    if (tr) *tr = v[3];
    if (bright) *bright = v[4];
    if (dark) *dark = v[5];
}

/* ---- groups -------------------------------------------------------------------------------------- */
BtnGroup *btn_group_get(int stage, int idx)
{
    if (stage < 0 || stage >= BTN_STAGES || idx < 0 || idx >= BTNGROUP_CNT) return NULL;
    return &g_grp[stage][idx];
}

void btn_group_init(BtnGroup *g)       /* FUN_005b94b0 */
{
    g->active = g->wait = g->cancel_ok = false;
    g->cancel_se = -1;
    g->decided = -2;
    g->result = g->result_no = 0;
    g->hit_no = g->pushed_no = -1;
}

void btn_group_start(BtnGroup *g)      /* sub 2 */
{
    input_clear();
    btn_group_init(g);
    g->active = true;
}

void btn_group_end(BtnGroup *g)        /* sub 5 */
{
    g->active = false;
    g->decided = -2;
}

static int stage_root(int stage) { return stage == 0 ? 37 : stage == 2 ? 73 : 38; }

void btn_group_sel(int stage, int idx, bool cancel_ok, int cancel_se)   /* subs 0 / 1 */
{
    BtnGroup *g = btn_group_get(stage, idx);
    if (!g) return;
    btn_init();
    input_clear();
    btn_group_init(g);
    g->wait = g->active = true;
    g->cancel_ok = cancel_ok;
    g->cancel_se = cancel_se;
    Proc p = { .type = PROC_BTNSEL };
    p.chain[0] = stage_root(stage); p.chain[1] = 6; p.chain[2] = -1; p.chain[3] = idx;
    p.nchain = 4;
    p.push_result = 1;
    p.iarg[0] = stage;
    p.iarg[1] = idx;
    proc_push(&p);
    if (log_trace_on("PROC")) log_trace("PROC", "push 0x10 group=%d.%d cancel=%d", stage, idx, (int)cancel_ok);
}

static void sel_done(int value)       /* the proc pushes the result and pops itself */
{
    if (g_done) { g_done(value); return; }
    Proc *t = proc_top();
    if (!t || t->type != PROC_BTNSEL) return;
    vm_push_int(value);
    proc_pop();
    if (log_trace_on("PROC")) log_trace("PROC", "pop 0x10 ret=%d", value);
}

static void btn_group_decide(int stage, int idx, int no)    /* FUN_005b9690 */
{
    BtnGroup *g = btn_group_get(stage, idx);
    if (!g || !g->active) return;
    g->active = false;
    g->decided = no;
    g->result = 1;
    g->result_no = no;
    g->hit_no = g->pushed_no = -1;
    if (log_trace_on("INPUT")) log_trace("INPUT", "button decide group=%d.%d no=%d", stage, idx, no);
    if (g->wait) sel_done(no);
}

/* Proc 0x10 (FUN_005a3080): waits for the decide (resolved in btn_frame); with cancel_ok a cancel
 * click ends it with -1 [I, ENGINE_SPEC §1.6]. */
bool btn_proc_sel(Proc *p)
{
    BtnGroup *g = btn_group_get(p->iarg[0], p->iarg[1]);
    if (!g) { sel_done(-1); return true; }
    if (!g->cancel_ok || !input_cancel_clicked()) return false;
    input_consume_cancel();
    g->active = false;
    g->decided = -2;
    sel_done(-1);
    return true;
}

void btn_init(void)
{
    if (g_inited) return;
    g_inited = true;
    proc_register(PROC_BTNSEL, btn_proc_sel);
    for (int s = 0; s < BTN_STAGES; s++)
        for (int i = 0; i < BTNGROUP_CNT; i++) btn_group_init(&g_grp[s][i]);
}

void btn_reset_all(void)
{
    for (int s = 0; s < BTN_STAGES; s++)
        for (int i = 0; i < BTNGROUP_CNT; i++) btn_group_init(&g_grp[s][i]);
    g_press = NULL;
}

/* ---- objects ------------------------------------------------------------------------------------- */
void btn_set(Obj *o, int no, int group, int action, int se)
{
    if (!o) return;
    o->is_button = true;
    o->btn_no = no;
    o->btn_group = group;
    o->btn_action = action;
    o->btn_se = se;
    o->btn_state = BTN_NORMAL;
}

void btn_test_hooks(BtnObjSource src, BtnHitFn hit, BtnSelDone done)
{
    g_src = src;
    g_hit = hit;
    g_done = done;
    g_press = NULL;
}

static int root_count(void)
{
    static int cnt = -1;
    if (cnt < 0) { int n = gx_int("OBJECT.CNT", 0, GFX_OBJ_CNT); cnt = n < 0 ? 0 : n > GFX_OBJ_CNT ? GFX_OBJ_CNT : n; }
    return cnt;
}

typedef struct { Obj *v[MAX_BTN]; bool vis[MAX_BTN]; int n; } BtnList;

/* NOLINTNEXTLINE(misc-no-recursion): tree depth capped at GFX_OBJ_DEPTH_MAX (CODE_STANDARD §6) */
static void collect(BtnList *l, Obj *o, bool parent_vis, int depth)
{
    if (!o || depth > GFX_OBJ_DEPTH_MAX) return;
    bool vis = parent_vis && o->disp && o->type != 0;
    if (o->is_button && l->n < MAX_BTN) { l->v[l->n] = o; l->vis[l->n] = vis; l->n++;
    } else if (o->is_button) {
        static bool once;
        if (!once) {
            once = true;
            log_msg(LOG_WARN, "gfx: more than %d buttons, the rest ignored", MAX_BTN);
        }
    }
    for (int k = 0; k < o->nchild; k++) collect(l, &o->child[k], parent_vis && o->disp, depth + 1);
}

static void collect_front(BtnList *l)
{
    l->n = 0;
    if (g_src) {
        for (int i = 0; i < BTN_SOURCE_MAX; i++) {
            Obj *o = g_src(i);
            if (!o) break;
            collect(l, o, true, 0);
        }
        return;
    }
    int n = root_count();
    for (int i = 0; i < n; i++) collect(l, gfx_obj(STAGE_FRONT, i), true, 0);
}

/* Objects of a group take part only while the group is active; group-less buttons always do (FUN_005fb910). */
static BtnGroup *obj_group(const Obj *o, bool *live)
{
    BtnGroup *g = (o->btn_group >= 0) ? btn_group_get(STAGE_FRONT, o->btn_group) : NULL;
    *live = g ? g->active : o->btn_group < 0;
    return g;
}

void btn_frame(void)
{
    static BtnList l;
    for (int i = 0; i < BTNGROUP_CNT; i++)
        if (g_grp[STAGE_FRONT][i].active) g_grp[STAGE_FRONT][i].hit_no = g_grp[STAGE_FRONT][i].pushed_no = -1;

    collect_front(&l);
    int mx = input_mouse_x(), my = input_mouse_y();
    const InputBtn *L = input_btn_mouse(0);
    Obj *hov = NULL;
    bool press_alive = false;
    for (int i = 0; i < l.n; i++) {
        Obj *o = l.v[i];
        bool live;
        obj_group(o, &live);
        if (o == g_press) press_alive = live;
        if (!live || !l.vis[i]) continue;
        if (g_hit ? g_hit(o, mx, my) : gfx_obj_hit(o, mx, my)) hov = o;   /* later = drawn on top */
    }
    if (!press_alive) g_press = NULL;
    if (L->on_down == 1 && hov) g_press = hov;

    for (int i = 0; i < l.n; i++) {
        Obj *o = l.v[i];
        bool live;
        BtnGroup *g = obj_group(o, &live);
        o->btn_state = BTN_NORMAL;
        if (!live || o != hov) continue;
        if (L->is_down && g_press == o) {
            o->btn_state = BTN_PUSH;
            if (g) g->pushed_no = o->btn_no;
        } else if (!L->is_down) {
            o->btn_state = BTN_HIT;
            if (g) g->hit_no = o->btn_no;
        }
    }

    /* decide = press and release on the same button (FUN_005ae0c0 → FUN_005b9690) */
    if (L->on_up == 1 && g_press && g_press == hov) {
        Obj *o = hov;
        g_press = NULL;
        input_consume_decide();
        if (o->btn_group >= 0) btn_group_decide(STAGE_FRONT, o->btn_group, o->btn_no);
    }
    if (!L->is_down) g_press = NULL;
}

/* ---- headless policy: drawn rect of a front-stage button ------------------------------------------- */

static void obj_origin(const Obj *o, int *px, int *py)
{
    int x = 0, y = 0;
    for (const Obj *p = o; p; p = p->parent) {
        x = core_wrap_add_i32(x, p->x.cur);
        y = core_wrap_add_i32(y, p->y.cur);
        for (int k = 0; k < p->nx_rep; k++) x = core_wrap_add_i32(x, p->x_rep[k].cur);
        for (int k = 0; k < p->ny_rep; k++) y = core_wrap_add_i32(y, p->y_rep[k].cur);
    }
    *px = x;
    *py = y;
}

bool btn_rect(int group, int no, Rect *out)
{
    static BtnList l;
    collect_front(&l);
    for (int i = 0; i < l.n; i++) {
        Obj *o = l.v[i];
        if (!l.vis[i] || o->btn_group != group || o->btn_no != no) continue;
        Rect r = { 0, 0, 0, 0 };
        const Image *img = o->type == OBJ_IMAGE ? img_get_ws(&o->file, o->patno.cur) : NULL;
        if (img) {
            int px, py;
            obj_origin(o, &px, &py);
            r = rect_at((int64_t)px - o->center_x.cur - img->cx, (int64_t)py - o->center_y.cur - img->cy, img->w,
                        img->h);   /* script positions: no int overflow */
        } else {                          /* rect objects / unknown geometry: probe the hit test */
            bool any = false;
            for (int y = 0; y < SCREEN_H; y += 4)
                for (int x = 0; x < SCREEN_W; x += 4) {
                    if (!(g_hit ? g_hit(o, x, y) : gfx_obj_hit(o, x, y))) continue;
                    if (!any) { r.x0 = x; r.y0 = y; r.x1 = x + 1; r.y1 = y + 1; any = true; }
                    if (x < r.x0) r.x0 = x;
                    if (y < r.y0) r.y0 = y;
                    if (x + 1 > r.x1) r.x1 = x + 1;
                    if (y + 1 > r.y1) r.y1 = y + 1;
                }
            if (!any) continue;
        }
        if (out) *out = r;
        return true;
    }
    return false;
}

/* ---- save / load (group state is local; objects carry their own button fields) ------------------- */
void btn_save(SavWriter *w)
{
    sav_put_chunk(w, FOURCC('B', 'T', 'N', 'G'));
    for (int s = 0; s < BTN_STAGES; s++)
        for (int i = 0; i < BTNGROUP_CNT; i++) {
            const BtnGroup *g = &g_grp[s][i];
            sav_put_u8(w, (uint8_t)(g->active | g->wait << 1 | g->cancel_ok << 2));
            sav_put_i32(w, g->cancel_se);
            sav_put_i32(w, g->decided);
            sav_put_i32(w, g->result);
            sav_put_i32(w, g->result_no);
        }
}

bool btn_load(SavReader *r)
{
    if (!sav_find_chunk(r, FOURCC('B', 'T', 'N', 'G'))) return false;
    for (int s = 0; s < BTN_STAGES; s++)
        for (int i = 0; i < BTNGROUP_CNT; i++) {
            BtnGroup *g = &g_grp[s][i];
            uint8_t f = sav_get_u8(r);
            g->active = f & 1;
            g->wait = (f >> 1) & 1;
            g->cancel_ok = (f >> 2) & 1;
            g->cancel_se = sav_get_i32(r);
            g->decided = sav_get_i32(r);
            g->result = sav_get_i32(r);
            g->result_no = sav_get_i32(r);
            g->hit_no = g->pushed_no = -1;
        }
    g_press = NULL;
    return sav_ok(r);
}
