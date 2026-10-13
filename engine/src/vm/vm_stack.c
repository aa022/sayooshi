/* vm/vm_stack.c — the value stacks of the interpreter (int stack, str stack, element marks; VM §2) and the
 * per-instruction argument pool (FUN_005a8fd0).
 *
 * Memory: fixed-capacity static arrays (VM_ISTACK_MAX, VM_SSTACK_MAX, VM_MARK_MAX, VM_ARG_MAX, VM_SUBLIST_MAX).
 * The WStr buffers of str-stack entries and argument slots are kept for reuse (tracked heap) and released by
 * vm_stacks_shutdown. Overflow and underflow are vm_fault()s: the script halts, nothing is written out of range. */
#include "vm/vm_internal.h"
#include "core/log.h"
#include <string.h>

static int32_t g_is[VM_ISTACK_MAX];
static int     g_isp;
static WStr    g_ss[VM_SSTACK_MAX];
static int     g_ssp;
static int     g_mk[VM_MARK_MAX];             /* int stack depth at each ELM_POINT, non-decreasing */
static int     g_nmk;

static Arg     g_arg[VM_ARG_MAX];
static int     g_argtop;
static ArgList g_sub[VM_SUBLIST_MAX];
static int     g_subtop;
static Arg     g_rhs;

VmDepths vm_depths(void) { return (VmDepths){ g_isp, g_ssp, g_nmk }; }

void vm_stacks_clear(void) { g_isp = g_ssp = g_nmk = 0; }

/* ---- int stack ---------------------------------------------------------------------------------------------- */

void vm_push_int(int32_t v)
{
    if (g_isp >= VM_ISTACK_MAX) { vm_fault("VMError", "int stack overflow (%d entries)", VM_ISTACK_MAX); return; }
    g_is[g_isp++] = v;
}

int32_t vm_pop_int(void)
{
    if (g_isp <= 0) { vm_fault("VMError", "int stack underflow"); return 0; }
    return g_is[--g_isp];
}

bool vm_dup_int(void)
{
    if (g_isp <= 0) { vm_fault("IndexError", "list index out of range"); return false; }
    vm_push_int(g_is[g_isp - 1]);
    return true;
}

/* ---- str stack ---------------------------------------------------------------------------------------------- */

WStr *vm_str_push_slot(void)
{
    if (g_ssp >= VM_SSTACK_MAX) { vm_fault("VMError", "str stack overflow (%d entries)", VM_SSTACK_MAX); return NULL; }
    return &g_ss[g_ssp++];
}

void vm_push_str(const WStr *s)
{
    WStr *d = vm_str_push_slot();
    if (d) ws_set(d, s ? s->p : NULL, s ? s->len : 0);
}

const WStr *vm_str_top(void) { return g_ssp > 0 ? &g_ss[g_ssp - 1] : NULL; }

static void ws_clear(WStr *s)
{
    s->len = 0;
    if (s->p) s->p[0] = 0;
}

bool vm_str_pop_swap(WStr *out)
{
    if (g_ssp <= 0) { vm_fault("VMError", "str stack underflow"); ws_clear(out); return false; }
    WStr t = g_ss[--g_ssp];
    g_ss[g_ssp] = *out;
    *out = t;
    return true;
}

void vm_pop_str(WStr *out)
{
    if (g_ssp <= 0) { vm_fault("VMError", "str stack underflow"); ws_set(out, NULL, 0); return; }
    const WStr *s = &g_ss[--g_ssp];
    ws_set(out, s->p, s->len);
}

bool vm_str_drop(void)
{
    if (g_ssp <= 0) { vm_fault("VMError", "str stack underflow"); return false; }
    g_ssp--;
    return true;
}

bool vm_dup_str(void)
{
    if (g_ssp <= 0) { vm_fault("IndexError", "list index out of range"); return false; }
    WStr *d = vm_str_push_slot();
    if (d) ws_copy(d, &g_ss[g_ssp - 2]);
    return d != NULL;
}

/* ---- element marks and chains ------------------------------------------------------------------------------- */

void vm_mark_push(void)
{
    if (g_nmk >= VM_MARK_MAX) { vm_fault("VMError", "element mark stack overflow (%d entries)", VM_MARK_MAX); return; }
    g_mk[g_nmk++] = g_isp;
}

void vm_push_ref(const int *chain, int n)
{
    vm_mark_push();
    for (int i = 0; i < n; i++) vm_push_int(chain[i]);
}

int vm_pop_chain(int out[VM_CHAIN_MAX])
{
    if (g_nmk <= 0) { vm_fault("VMError", "element mark stack underflow"); return 0; }
    int m = g_mk[--g_nmk];
    int n = g_isp - m;
    if (m < 0 || n < 0) { vm_fault("VMError", "element chain below mark"); return 0; }
    if (n > VM_CHAIN_MAX) { vm_fault("VMError", "element chain too long (%d)", n); g_isp = m; return 0; }
    memcpy(out, g_is + m, (size_t)n * sizeof *out);
    g_isp = m;
    return n;
}

bool vm_copy_elm(void)
{
    if (g_nmk <= 0) { vm_fault("IndexError", "list index out of range"); return false; }
    int m = g_mk[g_nmk - 1];
    if (m < 0) { vm_fault("VMError", "element chain below mark"); return false; }
    int n = g_isp - m;
    vm_mark_push();
    for (int k = 0; k < n && !vm_halted(); k++) vm_push_int(g_is[m + k]);
    return !vm_halted();
}

/* ---- save / load (part of VM02/VM01, docs/SAVE_FORMAT.md §9.1) ---------------------------------------------- */

void vm_stacks_save(SavWriter *w)
{
    sav_put_u32(w, (uint32_t)g_isp);
    for (int i = 0; i < g_isp; i++) sav_put_i32(w, g_is[i]);
    sav_put_u32(w, (uint32_t)g_ssp);
    for (int i = 0; i < g_ssp; i++) sav_put_wstr(w, &g_ss[i]);
    sav_put_u32(w, (uint32_t)g_nmk);
    for (int i = 0; i < g_nmk; i++) sav_put_i32(w, g_mk[i]);
}

/* A mark is an int stack depth taken from g_isp, so a negative one is corrupt data. (A mark above the current
 * depth can occur in odd but valid states; vm_pop_chain faults on it when it is used.) */
static bool load_marks(SavReader *r)
{
    int n = sav_get_count(r, VM_MARK_MAX, 4);
    for (int i = 0; i < n && sav_ok(r); i++) {
        int32_t m = sav_get_i32(r);
        if (m < 0 || m > VM_ISTACK_MAX) { sav_fail(r); break; }
        g_mk[g_nmk++] = m;
    }
    return sav_ok(r);
}

bool vm_stacks_load(SavReader *r)
{
    vm_stacks_clear();
    int n = sav_get_count(r, VM_ISTACK_MAX, 4);
    for (int i = 0; i < n; i++) g_is[g_isp++] = sav_get_i32(r);
    n = sav_get_count(r, VM_SSTACK_MAX, 4);
    for (int i = 0; i < n && sav_ok(r); i++) if (!sav_get_wstr(r, &g_ss[g_ssp++])) return false;
    return sav_ok(r) && load_marks(r);
}

/* ---- argument pool ------------------------------------------------------------------------------------------ */

static void arg_reset(Arg *a)
{
    a->id = -1; a->form = ELM_FORM_INT; a->i = 0; a->nchain = 0; a->sub = NULL;
    ws_clear(&a->s);
}

Arg *vm_args_alloc(int n)
{
    if (n < 0 || n > VM_ARG_MAX - g_argtop) { vm_fault("VMError", "argument pool exhausted"); return NULL; }
    Arg *a = &g_arg[g_argtop];
    g_argtop += n;
    for (int k = 0; k < n; k++) arg_reset(&a[k]);
    return a;
}

ArgList *vm_sublist_alloc(void)
{
    if (g_subtop >= VM_SUBLIST_MAX) { vm_fault("VMError", "nested arglist pool exhausted"); return NULL; }
    ArgList *l = &g_sub[g_subtop++];
    l->v = NULL;
    l->n = 0;
    return l;
}

void vm_args_release(void) { g_argtop = 0; g_subtop = 0; }

Arg *vm_rhs(void)
{
    arg_reset(&g_rhs);
    return &g_rhs;
}

void vm_stacks_shutdown(void)
{
    for (int i = 0; i < VM_SSTACK_MAX; i++) ws_free(&g_ss[i]);
    for (int i = 0; i < VM_ARG_MAX; i++) ws_free(&g_arg[i].s);
    ws_free(&g_rhs.s);
    vm_stacks_clear();
    vm_args_release();
}
