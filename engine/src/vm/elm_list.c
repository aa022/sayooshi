/* vm/elm_list.c — the int and str list objects of the VM (VM §6.1/6.2; FUN_00660880 / FUN_006614c0): banks, call
 * L/K, list locals and scene props, plus the fixed int lists other modules expose (elm_intlist).
 *
 * Memory: list storage is on the tracked heap (MEM_VM) because scripts resize lists; every size is clamped to
 * [0, ELM_LIST_MAX] (a script asking for more gets a vm_err), and all lists together stay inside the MEM_VM heap
 * budget (core/mem.h): a request past it gets a vm_err and leaves the list empty (init) or unchanged (resize),
 * so a script cannot exhaust the machine's memory. Owners release lists with elm_intlist_free /
 * elm_strlist_free. Index loops are bounded by the list size; at most ELM_RANGE_ERRS errors are reported per side
 * of an out-of-range clear() range. */
#include "vm/vm_internal.h"
#include <string.h>

enum { ELM_RANGE_ERRS = 16 };

static int list_size(int n)
{
    if (n < 0) return 0;
    if (n > ELM_LIST_MAX) { vm_err("list size %d out of range (max %d)", n, ELM_LIST_MAX); return ELM_LIST_MAX; }
    return n;
}

/* ---- storage -------------------------------------------------------------------------------------------------- */

static void budget_refused(int n)
{
    vm_err("list of %d entries refused: the VM heap budget is exhausted", n);
}

/* Zeroed storage for n (> 0 allowed: 0 means one spare cell) cells of `each` bytes; *n becomes 0 when the budget
 * refuses the request. Never NULL: the one-cell fallback lies inside the budget's headroom. */
static void *cells_alloc(int *n, size_t each)
{
    void *p = mem_try_alloc_array((size_t)(*n ? *n : 1), each, MEM_VM);
    if (p) return p;
    budget_refused(*n);
    *n = 0;
    return mem_alloc_array(1, each, MEM_VM);
}

void elm_intlist_init(IntList *l, int n, bool resizable)
{
    n = list_size(n);
    l->w = cells_alloc(&n, sizeof *l->w);
    l->n = l->def = n;
    l->resizable = resizable;
}

void elm_intlist_free(IntList *l)
{
    mem_free(l->w);
    memset(l, 0, sizeof *l);
}

void elm_intlist_resize(IntList *l, int n)
{
    n = list_size(n);
    if (!l->w) { elm_intlist_init(l, n, l->resizable); return; }
    if (n > l->n) {
        int32_t *w = mem_try_realloc_array(l->w, (size_t)n, sizeof *l->w);
        if (!w) { budget_refused(n); return; }   /* the list keeps its size */
        l->w = w;
        memset(l->w + l->n, 0, (size_t)(n - l->n) * sizeof *l->w);
    }                                          /* shrinking keeps the block: nothing can fail */
    l->n = n;
}

void elm_intlist_zero(IntList *l)
{
    if (l->n > 0) memset(l->w, 0, (size_t)l->n * sizeof *l->w);
}

void elm_strlist_init(StrList *l, int n, bool resizable)
{
    n = list_size(n);
    l->s = cells_alloc(&n, sizeof *l->s);
    l->n = l->def = n;
    l->resizable = resizable;
}

void elm_strlist_free(StrList *l)
{
    for (int i = 0; i < l->n; i++) ws_free(&l->s[i]);
    mem_free(l->s);
    memset(l, 0, sizeof *l);
}

void elm_strlist_resize(StrList *l, int n)
{
    n = list_size(n);
    if (!l->s) { elm_strlist_init(l, n, l->resizable); return; }
    if (n > l->n) {
        WStr *s = mem_try_realloc_array(l->s, (size_t)n, sizeof *l->s);
        if (!s) { budget_refused(n); return; }   /* the list keeps its size */
        l->s = s;
        memset(l->s + l->n, 0, (size_t)(n - l->n) * sizeof *l->s);
    } else {
        for (int i = n; i < l->n; i++) ws_free(&l->s[i]);   /* shrinking keeps the block: nothing can fail */
    }
    l->n = n;
}

void elm_strlist_clear(StrList *l)
{
    for (int i = 0; i < l->n; i++) {
        l->s[i].len = 0;
        if (l->s[i].p) l->s[i].p[0] = 0;       /* keeps the WStr terminator invariant */
    }
}

static int32_t elm_carg_int(const ElmCall *c, int k)
{
    if (!c->arg || k < 0 || k >= c->nall) return 0;
    return c->arg[k].form == ELM_FORM_INT ? c->arg[k].i : 0;
}

/* ---- int list protocol ---------------------------------------------------------------------------------------- */

/* Bit-packed access: element i of width bits lives in word (i*bits)>>5 at shift (i*bits)&31 (VM §6.1). */
static bool il_pos(const IntList *l, int32_t i, int bits, int *wi, int *sh)
{
    int64_t off = (int64_t)i * bits;
    if (i < 0 || (off >> 5) >= l->n) {
        vm_err("int list index out of range [%d] bits=%d", i, bits);
        return false;
    }
    *wi = (int)(off >> 5);
    *sh = (int)(off & 31);
    return true;
}

static int32_t il_get(const IntList *l, int32_t i, int bits)
{
    int wi, sh;
    if (!il_pos(l, i, bits, &wi, &sh)) return 0;
    if (bits == 32) return l->w[wi];
    return (int32_t)(((uint32_t)l->w[wi] >> sh) & ((1u << bits) - 1));
}

static void il_set(IntList *l, int32_t i, int32_t v, int bits)
{
    int wi, sh;
    if (!il_pos(l, i, bits, &wi, &sh)) return;
    if (bits == 32) { l->w[wi] = v; return; }
    uint32_t m = ((1u << bits) - 1) << sh;
    l->w[wi] = (int32_t)(((uint32_t)l->w[wi] & ~m) | (((uint32_t)v << sh) & m));
}

/* clear(a, b[, v]) over [a, b]: every in-range element is set; out-of-range indices are reported (and skipped)
 * at most ELM_RANGE_ERRS times on each side, so a huge range from a script stays cheap. */
static void il_clear(IntList *l, int32_t a, int32_t b, int32_t v, int bits)
{
    int64_t cnt = (int64_t)l->n * 32 / bits;
    int64_t lo = a, hi = b;
    int64_t in_lo = lo > 0 ? lo : 0, in_hi = hi < cnt - 1 ? hi : cnt - 1;
    for (int64_t k = lo; k <= hi && k < 0 && k < lo + ELM_RANGE_ERRS; k++) il_set(l, (int32_t)k, v, bits);
    for (int64_t k = in_lo; k <= in_hi; k++) il_set(l, (int32_t)k, v, bits);
    int64_t hi_lo = lo > cnt ? lo : cnt;
    for (int64_t k = hi_lo; k <= hi && k < hi_lo + ELM_RANGE_ERRS; k++) il_set(l, (int32_t)k, v, bits);
}

static int bit_view(int sub)                   /* sub-elements selecting a bit view: 3=b1 4=b2 5=b4 7=b8 6=b16 */
{
    switch (sub) {
    case 3: return 1;
    case 4: return 2;
    case 5: return 4;
    case 7: return 8;
    case 6: return 16;
    default: return 0;
    }
}

static void il_index(IntList *l, const int *rest, int nrest, ElmCall *c, int bits)   /* [i] */
{
    if (nrest < 2) { vm_err("int list: missing index"); elm_push_default(c); return; }
    if (c->al_id == 0) vm_push_int(il_get(l, rest[1], bits));
    else if (c->al_id == 1) il_set(l, rest[1], elm_carg_int(c, 0), bits);
}

static void il_sets(IntList *l, const ElmCall *c, int bits)   /* sets(start, v0, v1, ...) */
{
    int32_t a = elm_carg_int(c, 0);
    for (int k = 1; k < c->npos; k++) il_set(l, core_wrap_add_i32(a, k - 1), elm_carg_int(c, k), bits);
}

static ElmResult intlist_elm(IntList *l, const int *rest, int nrest, ElmCall *c)
{
    int bits = 32;
    while (nrest > 0 && bit_view(rest[0])) { bits = bit_view(rest[0]); rest++; nrest--; }
    if (nrest <= 0) { vm_err("int list used as value"); return ELM_OK; }
    switch (rest[0]) {
    case -1: il_index(l, rest, nrest, c, bits); break;
    case 10:                                   /* init */
        elm_intlist_zero(l);
        if (l->resizable) elm_intlist_resize(l, l->def);
        break;
    case 2:                                    /* resize */
        if (l->resizable) elm_intlist_resize(l, elm_carg_int(c, 0));
        else vm_err("int list: resize of a fixed list");
        break;
    case 9: vm_push_int((int32_t)((int64_t)l->n * 32 / bits)); break;   /* get_size */
    case 8: il_clear(l, elm_carg_int(c, 0), elm_carg_int(c, 1), c->al_id != 0 ? elm_carg_int(c, 2) : 0, bits); break;
    case 1: il_sets(l, c, bits); break;
    default: vm_err("int list: bad sub-element %d", rest[0]); break;
    }
    return ELM_OK;
}

ElmResult elm_intlist_obj(IntList *l, const int *rest, int nrest, ElmCall *c) { return intlist_elm(l, rest, nrest, c); }

ElmResult elm_intlist(int32_t *w, int n, const int *rest, int nrest, ElmCall *c)
{
    IntList l = { w, n > 0 ? n : 0, n > 0 ? n : 0, false };
    return intlist_elm(&l, rest, nrest, c);
}

/* ---- str list protocol ---------------------------------------------------------------------------------------- */

static void sl_sets(StrList *l, const ElmCall *c)   /* sets(start, s0, s1, ...): unused by the game */
{
    int64_t a = elm_carg_int(c, 0);
    for (int k = 1; k < c->npos; k++) {
        int64_t i = a + k - 1;
        if (c->arg[k].form == ELM_FORM_STR && i >= 0 && i < l->n) ws_copy(&l->s[i], &c->arg[k].s);
    }
}

ElmResult elm_strlist_obj(StrList *l, const int *rest, int nrest, ElmCall *c)
{
    if (nrest <= 0) { vm_err("str list used as value"); return ELM_OK; }
    switch (rest[0]) {
    case -1: {
        int32_t i = nrest >= 2 ? rest[1] : -1;
        if (i >= 0 && i < l->n) return elm_str(&l->s[i], rest + 2, nrest - 2, c);
        vm_err("str list index out of range [%d]", i);
        if (c->ret_form == ELM_FORM_INT) vm_push_int(0);   /* FUN_006614c0 pushes by ret_form only (PROPERTY: 0) */
        else if (c->ret_form == ELM_FORM_STR) vm_push_str(NULL);
        break;
    }
    case 3:                                    /* init */
        elm_strlist_clear(l);
        if (l->resizable) elm_strlist_resize(l, l->def);
        break;
    case 2:                                    /* resize */
        if (l->resizable) elm_strlist_resize(l, elm_carg_int(c, 0));
        else vm_err("str list: resize of a fixed list");
        break;
    case 4: vm_push_int(l->n); break;         /* get_size */
    case 1: sl_sets(l, c); break;
    default: vm_err("str list: bad sub-element %d", rest[0]); break;
    }
    return ELM_OK;
}
