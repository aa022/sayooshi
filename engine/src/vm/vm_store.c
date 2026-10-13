/* vm/vm_store.c — the VM's storage: call frames and their locals (VM §1.3), the variable banks (VM §6.3), scene and
 * include props (VM §6.4/6.5) and the per-pack tables (scene names).
 *
 * Memory and lifetimes:
 *  - frames and the local pool are fixed-capacity static arrays (VM_FRAME_MAX, VM_LOCAL_MAX). The locals of frame d
 *    are the pool entries [loc0, loc0 + nloc), directly after those of frame d - 1: only the top frame declares.
 *  - the per-pack tables (scene names, the Prop records of every scene and include prop) are carved from one
 *    private arena sized exactly for the bound pack; vm_store_unbind destroys it.
 *  - list objects (banks, L/K, list props) and Prop strings live on the tracked heap because scripts resize them;
 *    vm_store_unbind frees them all. */
#include "vm/vm_internal.h"
#include "core/log.h"
#include "fmt/gameexe.h"
#include <string.h>

/* Bank roots (VM §6.3). Int: A B C D E F X G Z; str: S M ％namae ＊namae. */
static const int INT_BANK_ROOT[VM_INT_BANKS] = { ELM_ROOT_BANK_A, ELM_ROOT_BANK_B, ELM_ROOT_BANK_C, ELM_ROOT_BANK_D,
                                                 ELM_ROOT_BANK_E, ELM_ROOT_BANK_F, ELM_ROOT_BANK_X, ELM_ROOT_BANK_G,
                                                 ELM_ROOT_BANK_Z };
static const int STR_BANK_ROOT[VM_STR_BANKS] = { ELM_ROOT_BANK_S, ELM_ROOT_BANK_M, ELM_ROOT_NAMAE_LOCAL,
                                                 ELM_ROOT_NAMAE_GLOBAL };
enum { NAMAE_CNT = 26 + 26 * 26 };            /* 702 (SYS §7.1) */
enum { STORE_ARENA_ALLOCS = 5, STORE_ARENA_SLACK = 256 };   /* per-allocation alignment + debug guard room */

static struct {
    const Pack *pk;
    Arena arena;
    bool arena_live;
    int nscene;
    const char **name;                         /* [nscene] WTF-8 scene names */
    Prop *prop;                                /* [nprop_total]: scene props by scene, then the include props */
    int *prop0, *nprop;                        /* [nscene] first prop / count of each scene */
    int nprop_total, ninc;
    int flag_cnt, gflag_cnt, call_cnt;
} g_store;

static Frame   g_fr[VM_FRAME_MAX];
static int     g_depth, g_max_depth;
static Prop    g_loc[VM_LOCAL_MAX];
static IntList g_bi[VM_INT_BANKS];
static StrList g_bs[VM_STR_BANKS];

/* ---- props ---------------------------------------------------------------------------------------------------- */

void vm_prop_reset(Prop *p, int form, int size, int name)
{
    if (p->form == ELM_FORM_INTLIST) elm_intlist_free(&p->il);
    if (p->form == ELM_FORM_STRLIST) elm_strlist_free(&p->sl);
    p->form = form; p->name = name; p->i = 0; p->nchain = 0;
    p->s.len = 0;
    if (p->s.p) p->s.p[0] = 0;
    if (form == ELM_FORM_INTLIST) elm_intlist_init(&p->il, size, true);
    if (form == ELM_FORM_STRLIST) elm_strlist_init(&p->sl, size, true);
}

void vm_prop_free(Prop *p)
{
    vm_prop_reset(p, ELM_FORM_NONE, 0, -1);
    ws_free(&p->s);
}

/* ---- per-pack tables ------------------------------------------------------------------------------------------ */

static size_t name_bytes(const WStr *w) { return (size_t)(w->len > 0 ? w->len : 0) * 3 + 1; }   /* WTF-8 bound */

typedef struct TableSize { size_t names, total; int props; } TableSize;

/* Arena bytes for the tables of pk; false when the pack exceeds the VM's limits. */
static bool tables_size(const Pack *pk, TableSize *ts)
{
    *ts = (TableSize){ 0, 0, pk->ninc_prop > 0 ? pk->ninc_prop : 0 };
    for (int s = 0; s < pk->nscene; s++) {
        const Scene *sc = &pk->scene[s];
        ts->names += name_bytes(pack_scene_name(pk, s));
        if (sc->nprop > 0) ts->props += sc->nprop;
        if (ts->props > VM_PROP_MAX) {
            log_msg(LOG_ERR, "vm_init: more than %d scene props", VM_PROP_MAX);
            return false;
        }
    }
    size_t per_scene = sizeof(char *) + 2 * sizeof(int);
    return core_mul_sz((size_t)pk->nscene, per_scene, &ts->total) && core_add_sz(ts->total, ts->names, &ts->total) &&
           core_add_sz(ts->total, (size_t)(ts->props + 1) * sizeof(Prop), &ts->total) &&
           core_add_sz(ts->total, (size_t)STORE_ARENA_ALLOCS * STORE_ARENA_SLACK, &ts->total);
}

static void tables_names(const Pack *pk, size_t bytes)
{
    char *buf = arena_alloc_uninit(&g_store.arena, bytes, 1);
    size_t pos = 0;
    for (int s = 0; s < pk->nscene; s++) {
        const WStr *w = pack_scene_name(pk, s);
        StrBuf b;
        sb_init(&b, buf + pos, name_bytes(w));
        (void)ws_to_utf8(w, &b);               /* the capacity is the WTF-8 bound: never truncated */
        g_store.name[s] = buf + pos;
        pos += b.n + 1;
    }
}

static void tables_props(const Pack *pk)
{
    int k0 = 0;
    for (int s = 0; s < pk->nscene; s++) {     /* scene props persist for the session (VM §6.5) */
        const Scene *sc = &pk->scene[s];
        g_store.prop0[s] = k0;
        g_store.nprop[s] = sc->nprop > 0 ? sc->nprop : 0;
        for (int k = 0; k < g_store.nprop[s]; k++)
            vm_prop_reset(&g_store.prop[k0 + k], sc->prop[k].form, sc->prop[k].size, k);
        k0 += g_store.nprop[s];
    }
    g_store.ninc = pk->ninc_prop > 0 ? pk->ninc_prop : 0;   /* 0 in this game; the pack does not expose their forms */
    for (int k = 0; k < g_store.ninc; k++) vm_prop_reset(&g_store.prop[k0 + k], ELM_FORM_INT, 0, k);
}

static bool tables_build(const Pack *pk)
{
    TableSize ts;
    if (!tables_size(pk, &ts)) return false;
    arena_init(&g_store.arena, "vm.pack", ts.total);
    g_store.arena_live = true;
    g_store.nscene = pk->nscene;
    g_store.name = ARENA_NEW_ARRAY(&g_store.arena, const char *, (size_t)pk->nscene);
    g_store.prop0 = ARENA_NEW_ARRAY(&g_store.arena, int, (size_t)pk->nscene);
    g_store.nprop = ARENA_NEW_ARRAY(&g_store.arena, int, (size_t)pk->nscene);
    g_store.prop = ARENA_NEW_ARRAY(&g_store.arena, Prop, (size_t)ts.props + 1);
    g_store.nprop_total = ts.props;
    tables_names(pk, ts.names);
    tables_props(pk);
    return true;
}

/* Gameexe list sizes (the game: FLAG 2000, GLOBAL_FLAG 1000, CALL_FLAG 50; SPEC_ASSETS). The caps keep a damaged
 * Gameexe from reserving gigabytes: the bank lists exist once, the call lists once per frame (VM_FRAME_MAX). */
enum { VM_FLAG_CNT_MAX = 1 << 16, VM_CALL_FLAG_CNT_MAX = 1 << 12 };

/* A Gameexe count, clamped to [0, max]. */
static int gx_count(const char *key, int def, int max)
{
    int n = gx_int(key, 0, def);
    if (n >= 0 && n <= max) return n;
    log_msg(LOG_WARN, "vm_init: #%s = %d out of range, using %d", key, n, n < 0 ? 0 : max);
    return n < 0 ? 0 : max;
}

static void banks_init(void)
{
    for (int b = 0; b < VM_INT_BANKS; b++)
        elm_intlist_init(&g_bi[b], b < VM_LOCAL_INT_BANKS ? g_store.flag_cnt : g_store.gflag_cnt, false);
    elm_strlist_init(&g_bs[VM_BANK_S], g_store.flag_cnt, false);
    elm_strlist_init(&g_bs[VM_BANK_M], g_store.gflag_cnt, false);
    elm_strlist_init(&g_bs[VM_BANK_NAMAE_LOCAL], NAMAE_CNT, false);
    elm_strlist_init(&g_bs[VM_BANK_NAMAE_GLOBAL], NAMAE_CNT, false);
}

bool vm_store_bind(const Pack *pk)
{
    vm_store_unbind();
    if (!tables_build(pk)) { vm_store_unbind(); return false; }
    g_store.pk = pk;
    g_store.flag_cnt = gx_count("FLAG.CNT", 2000, VM_FLAG_CNT_MAX);
    g_store.gflag_cnt = gx_count("GLOBAL_FLAG.CNT", 1000, VM_FLAG_CNT_MAX);
    g_store.call_cnt = gx_count("CALL_FLAG.CNT", 50, VM_CALL_FLAG_CNT_MAX);
    banks_init();
    vm_frames_reset();
    g_max_depth = 1;
    return true;
}

void vm_store_unbind(void)
{
    for (int i = 0; i < VM_FRAME_MAX; i++) { elm_intlist_free(&g_fr[i].L); elm_strlist_free(&g_fr[i].K); }
    memset(g_fr, 0, sizeof g_fr);
    for (int i = 0; i < VM_LOCAL_MAX; i++) vm_prop_free(&g_loc[i]);
    for (int b = 0; b < VM_INT_BANKS; b++) elm_intlist_free(&g_bi[b]);
    for (int b = 0; b < VM_STR_BANKS; b++) elm_strlist_free(&g_bs[b]);
    for (int k = 0; k < g_store.nprop_total; k++) vm_prop_free(&g_store.prop[k]);
    if (g_store.arena_live) arena_destroy(&g_store.arena);
    memset(&g_store, 0, sizeof g_store);
    g_depth = g_max_depth = 0;
}

const Pack *vm_pack(void) { return g_store.pk; }

const char *vm_scene_name_utf8(int scene)
{
    if (!g_store.pk || scene < 0 || scene >= g_store.nscene) return "";
    return g_store.name[scene];
}

int vm_find_scene(const WStr *name)
{
    if (!g_store.pk || !name) return -1;
    for (int i = 0; i < g_store.pk->nscene; i++)
        if (!ws_cmp_fold(pack_scene_name(g_store.pk, i), name)) return i;
    return -1;
}

int vm_find_scene_utf8(const char *name)
{
    uint16_t u[256];
    WBuf b;
    wb_init(&b, u, COUNT_OF(u));
    if (!wb_put_utf8(&b, str_c(name))) return -1;   /* invalid or longer than any scene name */
    WStr w = { u, (int)b.n, (int)b.cap };
    return vm_find_scene(&w);
}


/* ---- frames --------------------------------------------------------------------------------------------------- */

static void frame_reset(Frame *f, int type, int loc0)
{
    f->type = type; f->ret_form = 0;
    f->rs = (VmPos){ 0, 0, 0 };
    if (!f->L.w || f->L.n != g_store.call_cnt) {
        elm_intlist_free(&f->L);
        elm_intlist_init(&f->L, g_store.call_cnt, true);
    } else
        elm_intlist_zero(&f->L);
    if (!f->K.s || f->K.n != g_store.call_cnt) {
        elm_strlist_free(&f->K);
        elm_strlist_init(&f->K, g_store.call_cnt, true);
    } else
        elm_strlist_clear(&f->K);
    f->loc0 = loc0;
    f->nloc = 0;
    f->base = vm_depths();
}

static int next_loc0(void)
{
    const Frame *t = vm_frame_top();
    return t->loc0 + t->nloc;
}

Frame *vm_frame_top(void)
{
    ASSERT(g_depth >= 1 && g_depth <= VM_FRAME_MAX);
    return &g_fr[g_depth - 1];
}

Frame *vm_frame_at(int d)
{
    ASSERT(d >= 0 && d < g_depth);
    return &g_fr[d];
}

int vm_depth(void) { return g_depth; }
int vm_frame_max_depth(void) { return g_max_depth; }

void vm_frames_reset(void)
{
    g_depth = 1;
    frame_reset(&g_fr[0], 0, 0);
}

Frame *vm_frame_push(int type, int ret_form)   /* FUN_0059bcb0/0059bd40/005a4bd0 + FUN_00519c30 */
{
    if (g_depth >= VM_FRAME_MAX) { vm_fault("VMError", "call stack overflow (%d frames)", VM_FRAME_MAX); return NULL; }
    Frame *cur = vm_frame_top();
    cur->ret_form = ret_form;
    cur->rs = vm_pos();
    int loc0 = next_loc0();
    Frame *f = &g_fr[g_depth++];
    frame_reset(f, type, loc0);
    if (g_depth > g_max_depth) g_max_depth = g_depth;
    return f;
}

Frame *vm_frame_load(int d)
{
    if (d == 0) return vm_frame_top();
    ASSERT(d == g_depth);
    if (d >= VM_FRAME_MAX) return NULL;
    int loc0 = next_loc0();
    Frame *f = &g_fr[g_depth++];
    frame_reset(f, 0, loc0);
    return f;
}

void vm_frame_set_depth(int depth)
{
    ASSERT(depth >= 1 && depth <= g_depth);
    g_depth = depth;
}

void vm_frame_set_base(Frame *f) { f->base = vm_depths(); }

void vm_frame_pass_args(Frame *f, const Arg *a, int n)
{
    int ni = 0, ns = 0;
    for (int k = 0; k < n; k++) {
        if (a[k].form == ELM_FORM_INT) {
            if (ni < f->L.n) f->L.w[ni] = a[k].i;
            else vm_err("int list index out of range [%d] bits=32", ni);
            ni++;
        } else if (a[k].form == ELM_FORM_STR) {
            if (ns < f->K.n) ws_copy(&f->K.s[ns], &a[k].s);
            else vm_err("str list index out of range [%d]", ns);
            ns++;
        }
    }
}

Prop *vm_frame_add_local(Frame *f)
{
    ASSERT(f == vm_frame_top());
    int i = f->loc0 + f->nloc;
    if (i >= VM_LOCAL_MAX) return NULL;
    f->nloc++;
    return &g_loc[i];
}

Prop *vm_frame_local(const Frame *f, int i)
{
    ASSERT(i >= 0 && i < f->nloc && f->loc0 + i < VM_LOCAL_MAX);
    return &g_loc[f->loc0 + i];
}

/* ---- banks ---------------------------------------------------------------------------------------------------- */

IntList *vm_int_bank(int b) { ASSERT(b >= 0 && b < VM_INT_BANKS); return &g_bi[b]; }
StrList *vm_str_bank(int b) { ASSERT(b >= 0 && b < VM_STR_BANKS); return &g_bs[b]; }

int vm_int_bank_index(int root)
{
    for (int b = 0; b < VM_INT_BANKS; b++) if (INT_BANK_ROOT[b] == root) return b;
    return -1;
}

int vm_str_bank_index(int root)
{
    for (int b = 0; b < VM_STR_BANKS; b++) if (STR_BANK_ROOT[b] == root) return b;
    return -1;
}

void vm_local_banks_reset(void)
{
    for (int b = 0; b < VM_LOCAL_INT_BANKS; b++) elm_intlist_zero(&g_bi[b]);   /* A..F, X */
    elm_strlist_clear(&g_bs[VM_BANK_S]);
    elm_strlist_clear(&g_bs[VM_BANK_NAMAE_LOCAL]);
}

int32_t *vm_bank_int(int root, int idx)
{
    int b = vm_int_bank_index(root);
    if (b < 0) { log_msg(LOG_WARN, "vm_bank_int: bad root %d", root); return NULL; }
    if (idx < 0 || idx >= g_bi[b].n) { log_msg(LOG_WARN, "vm_bank_int: %d[%d] out of range", root, idx); return NULL; }
    return &g_bi[b].w[idx];
}

WStr *vm_bank_str(int root, int idx)
{
    int b = vm_str_bank_index(root);
    if (b < 0) { log_msg(LOG_WARN, "vm_bank_str: bad root %d", root); return NULL; }
    if (idx < 0 || idx >= g_bs[b].n) { log_msg(LOG_WARN, "vm_bank_str: %d[%d] out of range", root, idx); return NULL; }
    return &g_bs[b].s[idx];
}

/* ---- scene / include props ------------------------------------------------------------------------------------ */

int vm_scene_prop_count(int scene)
{ return g_store.pk && scene >= 0 && scene < g_store.nscene ? g_store.nprop[scene] : 0; }

Prop *vm_scene_prop(int scene, int k)
{
    ASSERT(k >= 0 && k < vm_scene_prop_count(scene));
    return &g_store.prop[g_store.prop0[scene] + k];
}

const PropDecl *vm_scene_prop_decl(int scene, int k)
{
    ASSERT(k >= 0 && k < vm_scene_prop_count(scene));
    return &g_store.pk->scene[scene].prop[k];
}

void vm_scene_prop_redeclare(int scene, int k)
{
    const PropDecl *d = vm_scene_prop_decl(scene, k);
    vm_prop_reset(vm_scene_prop(scene, k), d->form, d->size, k);
}

int vm_inc_prop_count(void) { return g_store.ninc; }

Prop *vm_inc_prop(int k)
{
    if (k < 0 || k >= g_store.ninc) return NULL;
    return &g_store.prop[g_store.nprop_total - g_store.ninc + k];
}
