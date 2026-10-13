/* vm/cgflags.c — root 78 cgtable and the CG flags it exposes (SYS §6.3; FUN_00662b50): a global int list,
 * saved in global data, set when a CG image is created (gfx_set_cg_hook) and read by the _menu CG mode.
 *
 * State: a fixed static array of CGTABLE_FLAG_MAX entries; nothing is allocated. */
#include "vm/sysstate.h"
#include "vm/vm_internal.h"
#include "core/log.h"
#include "fmt/cgtable.h"
#include "fmt/gameexe.h"
#include "sav/sav_stream.h"

enum { CG_SAVE_MAX = 1 << 20 };

/* ---- 78 cgtable (FUN_00662b50; flags = global int list, SYS §6.3) --------------------------------------------- */
static int32_t g_cg[CGTABLE_FLAG_MAX];
static int g_cg_cnt = -1;
static bool g_cg_disabled;                     /* 78.7/.8 (L+0x46d) */

static int32_t *cg_flags(int *count)
{
    if (g_cg_cnt < 0) {
        g_cg_cnt = gx_int("CGTABLE_FLAG_CNT", 0, CGTABLE_FLAG_MAX);
        if (g_cg_cnt < 0 || g_cg_cnt > CGTABLE_FLAG_MAX) g_cg_cnt = CGTABLE_FLAG_MAX;
    }
    *count = g_cg_cnt;
    return g_cg;
}

static int32_t *cg_flag_by_name(const char *name)
{
    int n, cnt;
    int32_t *f = cg_flags(&cnt);
    if (!name || (n = cgt_flag_no(name)) < 0 || n >= cnt) return NULL;
    return &f[n];
}

void sys_cg_mark_flag(int flag_no)
{
    int cnt;
    int32_t *f = cg_flags(&cnt);
    if (!g_cg_disabled && flag_no >= 0 && flag_no < cnt) f[flag_no] = 1;
}

void sys_cg_save_global(SavWriter *w)
{
    int cnt;
    const int32_t *f = cg_flags(&cnt);
    sav_put_chunk(w, FOURCC('C', 'G', 'T', 'F'));
    sav_put_i32(w, cnt);
    for (int i = 0; i < cnt; i++) sav_put_i32(w, f[i]);
}

bool sys_cg_load_global(SavReader *r)
{
    if (!sav_find_chunk(r, FOURCC('C', 'G', 'T', 'F'))) return false;
    int cnt, n = sav_get_count(r, CG_SAVE_MAX, 4);
    int32_t *f = cg_flags(&cnt);
    if (!sav_ok(r)) return false;
    /* Merge (OR) instead of overwrite: an unlocked CG never becomes locked again by loading older data. */
    for (int i = 0; i < n; i++) { int32_t v = sav_get_i32(r); if (i < cnt && sav_ok(r) && v) f[i] = v; }
    return sav_ok(r);
}

/* The flag of the CG named by string argument k; *no = its flag number (-1 if unknown). */
static int32_t *cg_flag_arg(const ElmCall *c, int k, int *no)
{
    SB_ON_STACK(name, CGT_NAME_MAX * 4 + 1);
    *no = -1;
    if (!ws_to_utf8(elm_arg_str(c, k), &name) || !sb_ok(&name)) return NULL;   /* no CG has such a name */
    *no = cgt_flag_no(sb_cstr(&name));
    return cg_flag_by_name(sb_cstr(&name));
}

static int cg_seen_count(void)
{
    int seen = 0;
    for (int i = 0; i < cgt_count(); i++) { const int32_t *p = cg_flag_by_name(cgt_name(i)); if (p && *p) seen++; }
    return seen;
}

static void cg_name_of(const ElmCall *c)       /* name of the CG with flag number arg 0 */
{
    int no = elm_arg_int(c, 0, -1);
    WStr s = { 0 };
    for (int i = 0; i < cgt_count(); i++)
        if (cgt_flag_no(cgt_name(i)) == no) { ws_set_utf8(&s, cgt_name(i)); break; }
    elm_ret_str(c, &s);
    ws_free(&s);
}

static ElmResult cg_root(int root, const int *rest, int nrest, ElmCall *c)
{
    (void)root;
    if (nrest < 1) return ELM_UNKNOWN;
    int cnt, no, sub = rest[0];
    int32_t *f = cg_flags(&cnt), *p;
    switch (sub) {
    case ELM_CGTABLE_FLAG: return elm_intlist(f, cnt, rest + 1, nrest - 1, c); /* flag[]: _menu CG thumbnails */
    case ELM_CGTABLE_GET_FLAG_NO_BY_NAME:
        (void)cg_flag_arg(c, 0, &no);
        elm_ret_int(c, no);
        return ELM_OK; /* only the number is wanted */
    case ELM_CGTABLE_GET_LOOK_BY_NAME:
        p = cg_flag_arg(c, 0, &no);
        elm_ret_int(c, p ? *p : 0);
        return ELM_OK;
    case ELM_CGTABLE_SET_LOOK_BY_NAME:
        p = cg_flag_arg(c, 0, &no);
        if (p) *p = elm_arg_int(c, 1, 0);
        elm_ret_none(c);
        return ELM_OK;
    case ELM_CGTABLE_GET_CG_CNT: elm_ret_int(c, cgt_count()); return ELM_OK;
    case ELM_CGTABLE_GET_LOOK_CNT: elm_ret_int(c, cg_seen_count()); return ELM_OK;
    case ELM_CGTABLE_GET_LOOK_PERCENT:
        elm_ret_int(c, cgt_count() > 0 ? cg_seen_count() * 100 / cgt_count() : 0);
        return ELM_OK;
    case ELM_CGTABLE_SET_DISABLE:
        g_cg_disabled = true;
        elm_ret_none(c);
        return ELM_OK;
    case ELM_CGTABLE_SET_ENABLE:
        g_cg_disabled = false;
        elm_ret_none(c);
        return ELM_OK;
    case ELM_CGTABLE_SET_ALL_FLAG: {
        int v = elm_arg_int(c, 0, 0);
        for (int i = 0; i < cgt_count(); i++) { p = cg_flag_by_name(cgt_name(i)); if (p) *p = v; }
        elm_ret_none(c);
        return ELM_OK;
    }
    case ELM_CGTABLE_GET_NAME_BY_FLAG_NO: cg_name_of(c); return ELM_OK;
    default: return ELM_UNKNOWN;
    }
}

void sys_cg_register(void)
{
    static const int ROOTS[] = { ELM_ROOT_CGTABLE };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), cg_root);
}
