/* vm/elements.c — the global element dispatcher (FUN_00659170), ElmCall argument helpers and unknown-chain
 * reporting (VM §4; ENGINE_SPEC §1.1, §1.5, §2.5). List objects: elm_list.c; str methods: elm_str.c; text
 * formatting: elm_fmt.c.
 *
 * Dispatch can re-enter itself (a reference argument is written back through elm_dispatch); the nesting is capped
 * at ELM_DISPATCH_DEPTH_MAX. The log-once set of unknown chains is a fixed open-addressing table. */
#include "vm/elements.h"
#include "vm/vm_internal.h"
#include "core/hash.h"
#include "core/log.h"

static const WStr g_empty;

/* ---- arguments ------------------------------------------------------------------------------------------------ */

int32_t elm_arg_int(const ElmCall *c, int k, int32_t def)
{
    if (!c || !c->arg || k < 0 || k >= c->npos) return def;
    const Arg *a = &c->arg[k];
    return a->form == ELM_FORM_INT ? a->i : def;
}

const WStr *elm_arg_str(const ElmCall *c, int k)
{
    if (!c || !c->arg || k < 0 || k >= c->npos) return &g_empty;
    const Arg *a = &c->arg[k];
    return a->form == ELM_FORM_STR ? &a->s : &g_empty;
}

int32_t elm_arg_raw_int(const ElmCall *c, int k, int32_t def)
{
    return (c && c->arg && k >= 0 && k < c->npos) ? c->arg[k].i : def;
}

const WStr *elm_arg_raw_str(const ElmCall *c, int k)
{
    return (c && c->arg && k >= 0 && k < c->npos) ? &c->arg[k].s : &g_empty;
}

const Arg *elm_arg_named(const ElmCall *c, int id)
{
    if (!c || !c->arg) return NULL;
    for (int k = c->npos; k < c->nall; k++)
        if (c->arg[k].id == id) return &c->arg[k];
    return NULL;
}

void elm_push_default(const ElmCall *c)
{
    if (!c) return;
    if (c->op == OP_PROPERTY) { vm_push_int(0); return; }   /* element type unknown: int keeps stacks sane */
    if (c->op != OP_COMMAND) return;
    if (c->ret_form == ELM_FORM_INT) vm_push_int(0);
    else if (c->ret_form == ELM_FORM_STR) vm_push_str(&g_empty);
}

/* ---- --vm-stubs mode ------------------------------------------------------------------------------------------ */

static bool g_vm_stubs;
static int  g_unk_count;
static int  g_dispatch_depth;

void elm_set_vm_stubs(bool on, int sel) { g_vm_stubs = on; vm_stub_set_sel(sel); }
bool elm_vm_stubs(void) { return g_vm_stubs; }
int  elm_unk_count(void) { return g_unk_count; }

/* ---- root owners (ENGINE_SPEC §1.1, §2.2; mwnd forwards per VM §9) --------------------------------------------- */

static ElmRootFn g_owner[ELM_ROOT_DISPATCH_MAX + 1];

void elm_register_roots(const int *roots, int n, ElmRootFn fn)
{
    for (int i = 0; i < n; i++) {
        int r = roots[i];
        ASSERT_MSG(r >= 0 && r <= ELM_ROOT_DISPATCH_MAX, "element root %d outside the dispatch table", r);
        ASSERT_MSG(!g_owner[r] || g_owner[r] == fn, "element root %d registered by two modules", r);
        g_owner[r] = fn;
    }
}

/* ---- results (ENGINE_SPEC §2.5: push exactly one value of ret_form, PROPERTY by element type) ---------------- */

void elm_ret_int(const ElmCall *c, int32_t v)
{
    if (c->op == OP_ASSIGN) return;
    if (c->op == OP_PROPERTY || c->ret_form == ELM_FORM_INT) vm_push_int(v);
    else if (c->ret_form) elm_push_default(c);
}

void elm_ret_str(const ElmCall *c, const WStr *s)
{
    if (c->op == OP_ASSIGN) return;
    if (c->op == OP_PROPERTY || c->ret_form == ELM_FORM_STR) vm_push_str(s);
    else if (c->ret_form) elm_push_default(c);
}

void elm_ret_none(const ElmCall *c) { if (c->op == OP_COMMAND && c->ret_form) elm_push_default(c); }

/* ENGINE_SPEC §1.5: whole roots that the game never uses; unknown chains under them log at debug only. */
static bool stub_whitelisted(int root)
{
    switch (root) {
    case 0: case 8: case 71: case 9: case 12: case 13: case 17: case 19: case 21: case 41:
    case 56: case 57: case 58: case 59: case 60: case 61: case 62: case 72: case 90: case 91: case 92:
    case 93: case 94: case 95: case 101: case 102: case 117: case 118: case 119: case 120: case 121:
    case 122: case 123: case 124: case 125: case 151: case 156: case 76: case 77: case 126: case 127:
    case 128: case 43: case 45: case 20: case 23: case 50: case 51: case 33: case 145: case 110:
    case 112: case 129: case 138: case 139: case 140: case 141: case 65: case 79:
        return true;
    default:
        return false;
    }
}

/* ---- unknown chains: log once per distinct chain -------------------------------------------------------------- */

enum { UNK_SET = 4096, UNK_LINE_MAX = 1024 };
static uint64_t g_unk_seen[UNK_SET];           /* FNV-1a of the chain ints; 0 = empty slot */

static bool unk_first(const int *chain, int n)
{
    uint64_t h = hash_fnv64(HASH_FNV64_INIT, chain, (size_t)n * sizeof *chain);
    if (!h) h = 1;
    for (unsigned k = 0; k < UNK_SET; k++) {   /* a hash collision only suppresses a log line */
        uint64_t *slot = &g_unk_seen[(h + k) & (UNK_SET - 1)];
        if (*slot == h) return false;
        if (!*slot) { *slot = h; return true; }
    }
    return false;
}

static void unk_report(const int *chain, int n, const ElmCall *c, const char *why)
{
    if (!unk_first(chain, n)) return;
    SB_ON_STACK(line, UNK_LINE_MAX);
    elm_fmt_call(&line, chain, n, c);
    const char *s = sb_cstr(&line), *scene = vm_scene_name_utf8(vm_scene());
    int root = chain[0];
    if (root >= 0 && root <= ELM_ROOT_DISPATCH_MAX && stub_whitelisted(root)) {
        log_msg(LOG_DEBUG, "stub [%s@0x%06x] %s", scene, vm_pc(), s);
        return;
    }
    g_unk_count++;
    log_msg(LOG_WARN, "UNK [%s@0x%06x] %s%s%s", scene, vm_pc(), s, why ? " :: " : "", why ? why : "");
    log_trace("UNK", "[%s@0x%06x] %s", scene, vm_pc(), s);
    if (vm_hooks()->on_unknown) vm_hooks()->on_unknown(s);
}

/* ---- the dispatcher ------------------------------------------------------------------------------------------- */

static ElmResult dispatch_root(int root, const int *rest, int nrest, ElmCall *c, const char **why)
{
    ElmRootFn owner = g_owner[root];
    if (owner) return owner(root, rest, nrest, c);
    *why = "no global case (FUN_00659170 default)";
    return ELM_UNKNOWN;
}

static ElmResult dispatch(const int *chain, int n, ElmCall *c)
{
    if (g_vm_stubs) return vm_stub_dispatch(chain, n, c);
    int root = chain[0];
    ElmResult r = ELM_UNKNOWN;
    const char *why = NULL;
    if (root >= 0 && root <= ELM_ROOT_DISPATCH_MAX) r = dispatch_root(root, chain + 1, n - 1, c, &why);
    else if (((uint32_t)root >> 24) == 0x7F) r = vm_user_prop(root & 0xFFFF, chain + 1, n - 1, c);
    else if (((uint32_t)root >> 24) == 0x7E) r = vm_user_cmd(root & 0xFFFF, c);
    else why = "invalid command (0xa1de1c)";
    if (r == ELM_UNKNOWN) {
        unk_report(chain, n, c, why);
        elm_push_default(c);
    }
    return r;
}

ElmResult elm_dispatch(const int *chain, int n, ElmCall *c)
{
    if (!chain || n <= 0) { vm_err("empty element chain"); return ELM_OK; }
    if (g_dispatch_depth >= ELM_DISPATCH_DEPTH_MAX) {
        vm_err("element dispatch nested deeper than %d", ELM_DISPATCH_DEPTH_MAX);
        elm_push_default(c);
        return ELM_OK;
    }
    g_dispatch_depth++;
    ElmResult r = dispatch(chain, n, c);
    g_dispatch_depth--;
    return r;
}
