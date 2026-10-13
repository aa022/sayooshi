/* vm/vm_ref.c — the vm_ref.py-compatible side of the VM (ENGINE_SPEC §3.1, M1): the --vm-stubs dispatcher
 * (vm_ref.py dispatch/h_stub), its log stream, the single-scene runner and the all-scenes sweep. The output must
 * stay byte-identical to vm_ref.py (tests/ref/vm_*.txt).
 *
 * Memory: the Counter tables (vm_ref.py collections.Counter clones) live on the tracked heap with a fixed entry
 * limit; sorting and the sweep's per-scene flags use scratch memory. vm_ref_shutdown frees everything. */
#include "vm/vm_internal.h"
#include "core/hash.h"
#include "core/log.h"
#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { CNT_MAX = 1 << 16, NLV_MAX = 5, NLV_LEN = 160, LOG_LINE_MAX = 2048, KEY_MAX = 256, CALL_MAX = 4096,
       REASON_MAX = 600, REPR_MAX = 1024, SEL_LIST_MAX = 2048, SEL_ITEM_MAX = 512, RESTART_MAX = 3,
       DEFAULT_MAX_STEPS = 2000000, SWEEP_MAX_STEPS = 300000, SWEEP_TOP = 60, RUN_TOP_ERRS = 8, RUN_TOP_STUBS = 12 };

/* ---- output stream: a failed write is logged once; the run's result does not depend on it ---------------------- */

static void out_fail(void)
{
    static bool logged;
    if (!logged) log_msg(LOG_ERR, "vm_ref: writing the output failed");
    logged = true;
}

static void out_fmt(FILE *o, const char *fmt, ...) CORE_PRINTF(2, 3);
static void out_fmt(FILE *o, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int k = vfprintf(o, fmt, ap);
    va_end(ap);
    if (k < 0) out_fail();
}

static void out_flush(FILE *o) { if (fflush(o) != 0) out_fail(); }

/* ---- Counter: insertion-ordered string counts with most_common() ---------------------------------------------- */

typedef struct { char *key; int64_t n; int order; } CntItem;
typedef struct { CntItem *v; int n, cap; int *tab; int tabcap; } Counter;

static uint32_t cnt_hash(const char *key) { return (uint32_t)hash_fnv64(HASH_FNV64_INIT, key, strlen(key)); }

static void cnt_free(Counter *c)
{
    for (int i = 0; i < c->n; i++) mem_free(c->v[i].key);
    mem_free(c->v);
    mem_free(c->tab);
    memset(c, 0, sizeof *c);
}

static void cnt_rehash(Counter *c, int cap)    /* cap: a power of two > 2 * n */
{
    int *t = mem_alloc_array((size_t)cap, sizeof *t, MEM_VM);
    for (int i = 0; i < cap; i++) t[i] = -1;
    for (int i = 0; i < c->n; i++) {
        uint32_t h = cnt_hash(c->v[i].key) & (uint32_t)(cap - 1);
        while (t[h] >= 0) h = (h + 1) & (uint32_t)(cap - 1);
        t[h] = i;
    }
    mem_free(c->tab);
    c->tab = t;
    c->tabcap = cap;
}

static void cnt_add_n(Counter *c, const char *key, int64_t times)
{
    if (c->n * 2 >= c->tabcap) cnt_rehash(c, c->tabcap ? c->tabcap * 2 : 256);
    uint32_t h = cnt_hash(key) & (uint32_t)(c->tabcap - 1);
    while (c->tab[h] >= 0) {                   /* the table is at most half full: terminates */
        if (!strcmp(c->v[c->tab[h]].key, key)) { c->v[c->tab[h]].n += times; return; }
        h = (h + 1) & (uint32_t)(c->tabcap - 1);
    }
    if (c->n >= CNT_MAX) {
        static bool logged;
        if (!logged) log_msg(LOG_WARN, "vm_ref: more than %d distinct counter keys; the rest are not counted", CNT_MAX);
        logged = true;
        return;
    }
    if (c->n == c->cap) {
        c->cap = c->cap ? c->cap * 2 : 64;
        c->v = mem_realloc_array(c->v, (size_t)c->cap, sizeof *c->v);
    }
    c->v[c->n] = (CntItem){ mem_strdup(key, MEM_VM), times, c->n };
    c->tab[h] = c->n++;
}

static void cnt_add(Counter *c, const char *key) { cnt_add_n(c, key, 1); }

static int cnt_cmp(const void *a, const void *b)   /* most_common: count desc, then insertion order */
{
    const CntItem *x = a, *y = b;
    if (x->n != y->n) return x->n > y->n ? -1 : 1;
    return x->order - y->order;
}

/* The items sorted like most_common(), in a's memory. */
static CntItem *cnt_sorted(const Counter *c, Arena *a)
{
    CntItem *s = ARENA_NEW_ARRAY(a, CntItem, (size_t)c->n + 1);
    if (c->n) memcpy(s, c->v, (size_t)c->n * sizeof *s);
    qsort(s, (size_t)c->n, sizeof *s, cnt_cmp);
    return s;
}

/* ---- state ---------------------------------------------------------------------------------------------------- */

static struct {
    FILE *out;                                 /* vm_ref log lines and TEXT lines; NULL = quiet */
    int sel;
    int restarts;
    int64_t n_jump, n_farcall, n_usercmd;
    Counter stubs, errmsg;
    char nlv[NLV_MAX][NLV_LEN];
    int nnlv;
} g_ref;

void vm_ref_reset(void)
{
    FILE *out = g_ref.out;
    int sel = g_ref.sel;
    cnt_free(&g_ref.stubs);
    cnt_free(&g_ref.errmsg);
    memset(&g_ref, 0, sizeof g_ref);
    g_ref.out = out;
    g_ref.sel = sel ? sel : 1;
}

void vm_ref_shutdown(void)
{
    cnt_free(&g_ref.stubs);
    cnt_free(&g_ref.errmsg);
    memset(&g_ref, 0, sizeof g_ref);
}

void vm_stub_set_sel(int sel) { g_ref.sel = sel; }
bool vm_ref_logging(void) { return g_ref.out != NULL; }

void vm_ref_log(const char *fmt, ...)
{
    if (!g_ref.out) return;
    char line[LOG_LINE_MAX];
    va_list ap;
    va_start(ap, fmt);
    str_vfmt_cut(line, sizeof line, fmt, ap);   /* cut at a code point if longer */
    va_end(ap);
    out_fmt(g_ref.out, "  [%s:%d] %s\n", vm_scene_name_utf8(vm_scene()), vm_line(), line);
}

void vm_ref_text(int rf, const char *speaker, bool has_speaker, const char *text)
{
    if (g_ref.out)
        out_fmt(g_ref.out, "TEXT  [%s:%d rf%d] %s%s%s\n", vm_scene_name_utf8(vm_scene()), vm_line(), rf, speaker,
                has_speaker ? ": " : "", text);
}

void vm_ref_count(VmRefEvent e)
{
    switch (e) {
    case VM_REF_JUMP: g_ref.n_jump++; break;
    case VM_REF_FARCALL: g_ref.n_farcall++; break;
    case VM_REF_USERCMD: g_ref.n_usercmd++; break;
    default: ASSERT_MSG(false, "vm_ref_count: bad event %d", (int)e);
    }
}

void vm_ref_note_err(const char *msg)
{
    if (g_ref.out || elm_vm_stubs()) cnt_add(&g_ref.errmsg, msg);
}

void vm_ref_note_nl(uint32_t at, VmDepths now, VmDepths base)
{
    if (g_ref.nnlv >= NLV_MAX) return;
    str_fmt_cut(g_ref.nlv[g_ref.nnlv++], NLV_LEN, "('%s', %u, %d, (%d, %d, %d), (%d, %d, %d))",   /* cut if longer */
                  vm_scene_name_utf8(vm_scene()), at, vm_line(), now.isp, now.ssp, now.nmk, base.isp, base.ssp,
                  base.nmk);
}

/* ---- the stub dispatcher -------------------------------------------------------------------------------------- */

static const char *op_name(int op)
{
    static const char *const OPN[3] = { "PROPERTY", "ASSIGN", "COMMAND" };
    return op >= 0 && op < 3 ? OPN[op] : "?";
}

static bool stub_vm_root(int root, int al)
{
    if (vm_int_bank_index(root) >= 0) return true;
    switch (root) {
    case 34: case 35: case 106: case 107: case 83: case 4: case 5: case 131: case 158: case 139: case 108: return true;
    case 3: return al == 0;
    default: return false;
    }
}

/* Roots whose stub calls vm_ref.py does not log. */
static bool stub_quiet(int root)
{
    return root == 84 || root == 14 || root == 10 || root == 11 || root == 55 || root == 103 || root == 115;
}

static void stub_select(const ElmCall *c, const char *ex)   /* selection: vm_ref pushes --sel */
{
    if (g_ref.out) {
        SB_ON_STACK(lst, SEL_LIST_MAX);
        bool first = true;
        (void)sb_put_char(&lst, '[');
        for (int i = 0; i < c->nall && c->arg; i++) {
            if (c->arg[i].form != ELM_FORM_STR) continue;
            SB_ON_STACK(one, SEL_ITEM_MAX);
            elm_repr_wstr(&one, &c->arg[i].s);
            (void)sb_fmt(&lst, "%s%s", first ? "" : ", ", sb_cstr(&one));   /* a long list is cut */
            first = false;
        }
        (void)sb_put_char(&lst, ']');
        vm_ref_log("SELECT %s -> %d%s", sb_cstr(&lst), g_ref.sel, ex);
    }
    vm_push_int(g_ref.sel);
}

static void stub_result(int root, const ElmCall *c)   /* what vm_ref.py pushes for a stubbed element */
{
    if (c->op == OP_PROPERTY) { vm_push_int(0); return; }
    if (c->op != OP_COMMAND) return;
    if (c->ret_form == ELM_FORM_INT) vm_push_int(0);
    else if (c->ret_form == ELM_FORM_STR) vm_push_str(NULL);
    else if (c->ret_form != 0) { int ch[2] = { -999, root }; vm_push_ref(ch, 2); }
}

ElmResult vm_stub_dispatch(const int *chain, int n, ElmCall *c)
{
    int root = chain[0];
    uint32_t hi = (uint32_t)root >> 24;
    if (root > 0xA3 && hi == 0x7F) return vm_user_prop(root & 0xFFFF, chain + 1, n - 1, c);
    if (root > 0xA3 && hi == 0x7E) return vm_user_cmd(root & 0xFFFF, c);
    if (stub_vm_root(root, c->al_id)) return vm_elm_global(root, chain + 1, n - 1, c);

    SB_ON_STACK(key, KEY_MAX);
    elm_fmt_chain(&key, chain, n);
    if (g_ref.out || elm_vm_stubs()) {
        SB_ON_STACK(sk, KEY_MAX + 44);
        (void)sb_fmt(&sk, "('%s', '%s')", op_name(c->op), sb_cstr(&key));
        cnt_add(&g_ref.stubs, sb_cstr(&sk));
    }
    char ex[32] = "";
    int32_t rf;
    if (c->op == OP_COMMAND && vm_inline_pending(&rf)) str_fmt_cut(ex, sizeof ex, " rf=%d", rf);   /* fits */
    if (c->op == OP_COMMAND && root == 100) { stub_select(c, ex); return ELM_OK; }
    stub_result(root, c);
    if (g_ref.out && c->op != OP_ASSIGN && !stub_quiet(root)) {
        SB_ON_STACK(call, CALL_MAX);
        elm_fmt_call(&call, chain, n, c);
        vm_ref_log("%s elm %s%s", op_name(c->op), sb_cstr(&call), ex);
    }
    return ELM_OK;
}

void vm_stub_restart(void)                     /* vm_ref.py root 3 al0 */
{
    vm_ref_log("root 3 al0: restart from #START_SCENE (_start z0); local banks reset");
    if (++g_ref.restarts > RESTART_MAX) { vm_stop("restart loop"); return; }
    vm_reset_stacks();
    vm_local_banks_reset();
    int idx = vm_find_scene_utf8("_start");
    if (idx < 0) { vm_stop("scene not found: _start"); return; }
    (void)vm_goto_z(idx, 0);                   /* z 0 is always a valid z label index */
}

/* ---- the runner ----------------------------------------------------------------------------------------------- */

static void print_item(FILE *o, const CntItem *it, bool quote_key)
{
    if (!quote_key) { out_fmt(o, "(%s, %" PRId64 ")", it->key, it->n); return; }
    WStr w = { 0 };
    ws_set_utf8(&w, it->key);                  /* the lenient decoder, as vm_ref's str() of the key */
    SB_ON_STACK(repr, REPR_MAX);
    elm_repr_wstr(&repr, &w);
    ws_free(&w);
    out_fmt(o, "(%s, %" PRId64 ")", sb_cstr(&repr), it->n);
}

static void print_most_common(FILE *o, const Counter *c, int k, bool quote_key)
{
    Scratch sc = mem_scratch_begin(NULL);
    const CntItem *s = cnt_sorted(c, sc.arena);
    out_fmt(o, "[");
    for (int i = 0; i < c->n && i < k; i++) {
        if (i) out_fmt(o, ", ");
        print_item(o, &s[i], quote_key);
    }
    out_fmt(o, "]");
    mem_scratch_end(&sc);
}

/* Runs to a stop; true when the script stopped on a VM error (the reason then starts with "VM ERROR"). */
static bool run_loop(const VmRefOpts *o, char *reason, size_t rn)
{
    int64_t max_steps = o->max_steps > 0 ? o->max_steps : DEFAULT_MAX_STEPS;
    str_fmt_cut(reason, rn, "step limit");
    while (vm_steps() < max_steps) {
        int64_t j0 = g_ref.n_jump;
        vm_step();
        if (vm_halted()) { str_fmt_cut(reason, rn, "%s", vm_halt_reason()); return vm_halt_kind() == VM_HALT_ERROR; }
        if (o->max_texts > 0 && vm_text_count() >= o->max_texts) {
            str_fmt_cut(reason, rn, "reached %" PRId64 " TEXT ops", o->max_texts);
            break;
        }
        if (o->stop_on_jump && g_ref.n_jump != j0) {
            str_fmt_cut(reason, rn, "jumped to scene %s", vm_scene_name_utf8(vm_scene()));   /* cut if longer */
            break;
        }
    }
    return false;
}

static bool start_named(const char *name, int z)
{
    int idx = vm_find_scene_utf8(name);
    if (idx < 0) return false;
    vm_reset_stacks();
    vm_clear_stop();
    (void)vm_goto_z(idx, z);                   /* a bad z keeps scene 0 at pc 0, as before */
    return true;
}

static void print_run_summary(FILE *out, const char *reason)
{
    VmDepths d = vm_depths();
    const VmStats *st = vm_stats();
    out_fmt(out, "STOP: %s\n", reason);
    out_fmt(out, "steps=%" PRId64 " texts=%" PRId64 " call_depth_max=%d istack=%d sstack=%d marks=%d frames=%d\n",
            st->steps, st->texts, vm_frame_max_depth(), d.isp, d.ssp, d.nmk, vm_depth());
    out_fmt(out, "NL stack-balance violations: %d [", st->nl_viol);
    for (int i = 0; i < g_ref.nnlv; i++) out_fmt(out, "%s%s", i ? ", " : "", g_ref.nlv[i]);
    out_fmt(out, "]\n");
    out_fmt(out, "errors: %d ", st->errs);
    print_most_common(out, &g_ref.errmsg, RUN_TOP_ERRS, true);
    out_fmt(out, "\nuser cmds: %" PRId64 " farcalls: %" PRId64 " jumps: %" PRId64 "\n", g_ref.n_usercmd,
            g_ref.n_farcall, g_ref.n_jump);
    out_fmt(out, "top stubs: ");
    print_most_common(out, &g_ref.stubs, RUN_TOP_STUBS, false);
    out_fmt(out, "\n");
}

int vm_ref_run(const Pack *pk, const VmRefOpts *o, FILE *out)
{
    if (!out) out = stdout;
    int sel = o->sel ? o->sel : 1;
    elm_set_vm_stubs(true, sel);
    g_ref.out = o->quiet ? NULL : out;
    int rc = 2;
    const char *name = o->scene ? o->scene : "_start";
    if (vm_init(pk)) {
        g_ref.sel = sel;
        if (start_named(name, o->z)) {
            char reason[REASON_MAX];
            bool error = run_loop(o, reason, sizeof reason);
            print_run_summary(out, reason);
            rc = error ? 1 : 0;
        } else {
            out_fmt(out, "unknown scene %s\n", name);
        }
    }
    out_flush(out);
    g_ref.out = NULL;                              /* the caller closes the stream */
    return rc;
}

/* ---- the sweep ------------------------------------------------------------------------------------------------ */

typedef struct SweepTotals { int64_t texts, steps; int nok, nbad; bool first_ok, any; } SweepTotals;

static bool sweep_scene(FILE *out, int i, const VmRefOpts *o, Counter *all, SweepTotals *t)
{
    char reason[REASON_MAX];
    bool error = run_loop(o, reason, sizeof reason);
    const VmStats *st = vm_stats();
    bool ok = !error && st->nl_viol == 0;
    if (!t->any) { t->any = true; t->first_ok = ok; }
    if (ok) t->nok++; else t->nbad++;
    t->texts += st->texts;
    t->steps += st->steps;
    out_fmt(out, "%-24s steps=%8" PRId64 " texts=%5" PRId64 " depth=%d nl_viol=%d errs=%d :: %s\n",
            vm_scene_name_utf8(i), st->steps, st->texts, vm_frame_max_depth(), st->nl_viol, st->errs, reason);
    for (int k = 0; k < g_ref.stubs.n; k++)
        cnt_add_n(all, g_ref.stubs.v[k].key, g_ref.stubs.v[k].n); /* insertion order kept */
    return ok;
}

static void sweep_summary(FILE *out, const SweepTotals *t, const bool *bad, int nscene, const Counter *all)
{
    out_fmt(out, "SUMMARY {");
    if (t->any && t->first_ok) {
        out_fmt(out, "'ok': %d, 'texts': %" PRId64 ", 'steps': %" PRId64 "", t->nok, t->texts, t->steps);
        if (t->nbad) out_fmt(out, ", 'bad': %d", t->nbad);
    } else {
        out_fmt(out, "'bad': %d, 'texts': %" PRId64 ", 'steps': %" PRId64 "", t->nbad, t->texts, t->steps);
        if (t->nok) out_fmt(out, ", 'ok': %d", t->nok);
    }
    out_fmt(out, "} bad: [");
    for (int i = 0, k = 0; i < nscene; i++) if (bad[i]) out_fmt(out, "%s'%s'", k++ ? ", " : "", vm_scene_name_utf8(i));
    out_fmt(out, "]\n");
    out_fmt(out, "stubbed element chains (op, chain): count\n");
    Scratch sc = mem_scratch_begin(NULL);
    const CntItem *s = cnt_sorted(all, sc.arena);
    for (int k = 0; k < all->n && k < SWEEP_TOP; k++) out_fmt(out, "  %7" PRId64 " %s\n", s[k].n, s[k].key);
    mem_scratch_end(&sc);
}

int vm_ref_sweep(const Pack *pk, int64_t max_steps, int sel, FILE *out)
{
    if (!out) out = stdout;
    if (!pk || pk->nscene <= 0) return 2;
    elm_set_vm_stubs(true, sel ? sel : 1);
    Counter all = { 0 };
    SweepTotals t = { 0, 0, 0, 0, false, false };
    bool *bad = mem_alloc_array((size_t)pk->nscene, sizeof *bad, MEM_VM);
    VmRefOpts o = { NULL, 0, max_steps > 0 ? max_steps : SWEEP_MAX_STEPS, 0, sel ? sel : 1, true, true };
    int rc = 0;
    for (int i = 0; i < pk->nscene && rc == 0; i++) {
        g_ref.out = NULL;
        if (!vm_init(pk)) { rc = 2; break; }
        g_ref.sel = o.sel;
        (void)vm_goto_z(i, 0);                 /* z 0 is always a valid z label index */
        bad[i] = !sweep_scene(out, i, &o, &all, &t);
    }
    if (rc == 0) {
        sweep_summary(out, &t, bad, pk->nscene, &all);
        rc = t.nbad ? 1 : 0;
    }
    mem_free(bad);
    cnt_free(&all);
    out_flush(out);
    return rc;
}
