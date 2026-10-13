/* vm/vm.c — the bytecode interpreter (FUN_005a3680): code stream, position, opcodes, errors and halting, the run
 * loops, init/start/restart and shutdown (SPEC_VM, ENGINE_SPEC §2.4.2).
 *
 * The bytecode is untrusted: every read is checked against code_size, every label, z label and string index
 * against its table, and every stack access goes through vm_stack.c. A violation halts the script with
 * "VM ERROR <type>: ..." (vm_ref.py VMError wording) and the frame driver pushes PROC_HALT; nothing is read or
 * written out of range and the engine keeps running. */
#include "vm/vm_internal.h"
#include "core/bin.h"
#include "core/log.h"
#include "fmt/gameexe.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include <inttypes.h>
#include <stdarg.h>
#include <string.h>

enum {
    BC_END = 0x00, BC_NL = 0x01, BC_PUSH = 0x02, BC_POP = 0x03, BC_COPY = 0x04, BC_PROP = 0x05, BC_COPY_ELM = 0x06,
    BC_DEC_PROP = 0x07, BC_ELM_POINT = 0x08, BC_ARG = 0x09, BC_GOTO = 0x10, BC_GOTO_TRUE = 0x11, BC_GOTO_FALSE = 0x12,
    BC_GOSUB = 0x13, BC_GOSUB_STR = 0x14, BC_RETURN = 0x15, BC_EOF = 0x16, BC_ASSIGN = 0x20, BC_OP1 = 0x21,
    BC_OP2 = 0x22, BC_COMMAND = 0x30, BC_TEXT = 0x31, BC_NAME = 0x32
};
enum { VM_REASON_MAX = 512, VM_MSG_MAX = 256, VM_Z_MAX = 1000, VM_CMD_LINE_MAX = 4096, VM_STR_REPEAT_MAX = 1 << 20 };
enum { VM_RUN_GUARD = 50000000 };             /* instructions without a wait before vm_run yields the frame */

static struct {
    VmPos pos;
    VmStats st;
    bool stop;
    VmHaltKind halt;                           /* NONE while running; set with stop */
    char reason[VM_REASON_MAX];
    int32_t inline_val;
    bool inline_pending;
    WStr speaker;                              /* NAME text, kept for the TEXT trace line */
} g_vm;

/* ---- position and stream -------------------------------------------------------------------------------------- */

VmPos vm_pos(void) { return g_vm.pos; }

void vm_set_pos(VmPos p)
{
    ASSERT(vm_pack() && p.scene >= 0 && p.scene < vm_pack()->nscene);
    g_vm.pos = p;
}

void vm_jump(int scene, uint32_t pc)
{
    ASSERT(vm_pack() && scene >= 0 && scene < vm_pack()->nscene);
    g_vm.pos.scene = scene;
    g_vm.pos.pc = pc;
}

int vm_scene(void) { return g_vm.pos.scene; }
int vm_line(void) { return g_vm.pos.line; }
uint32_t vm_pc(void) { return g_vm.pos.pc; }
const VmStats *vm_stats(void) { return &g_vm.st; }
int64_t vm_steps(void) { return g_vm.st.steps; }
int64_t vm_text_count(void) { return g_vm.st.texts; }

static const Scene *cur_sc(void) { return &vm_pack()->scene[g_vm.pos.scene]; }

/* n more bytes of code at pc (pc itself may be anything a jump table produced). */
static bool code_has(uint32_t n)
{
    const Scene *sc = cur_sc();
    return g_vm.pos.pc <= sc->code_size && sc->code_size - g_vm.pos.pc >= n;
}

static int32_t rd32(void)
{
    if (!code_has(4)) { vm_fault("IndexError", "read past end of code"); return 0; }
    int32_t v = (int32_t)bin_load_le32(cur_sc()->code + g_vm.pos.pc);
    g_vm.pos.pc += 4;
    return v;
}

static uint8_t rd8(void)
{
    if (!code_has(1)) { vm_fault("IndexError", "read past end of code"); return BC_EOF; }
    return cur_sc()->code[g_vm.pos.pc++];
}

bool vm_goto_z(int scene, int32_t z)           /* FUN_0059bbe0 tail: z < 1000 (unsigned) */
{
    ASSERT(vm_pack() && scene >= 0 && scene < vm_pack()->nscene);
    if ((uint32_t)z >= VM_Z_MAX) return false;
    const Scene *sc = &vm_pack()->scene[scene];
    vm_jump(scene, sc->zlabel ? (uint32_t)vm_tab(sc->zlabel, z) : 0);
    return true;
}

static void jump_label(int32_t lab)            /* an invalid label falls through (FUN_0054c830) */
{
    const Scene *sc = cur_sc();
    if (sc->label && lab >= 0 && lab < sc->nlabel) g_vm.pos.pc = (uint32_t)vm_tab(sc->label, lab);
}

int32_t vm_read_inline_i32(void)
{
    if (!g_vm.inline_pending) { vm_err("inline read outside an inline-read command"); return -1; }
    return g_vm.inline_val;
}

bool vm_inline_pending(int32_t *v)
{
    *v = g_vm.inline_val;
    return g_vm.inline_pending;
}

/* SPEC_VM §9 deterministic rule: the commands followed by an inline read-flag word. */
static bool mwnd_sub(int x)
{
    return x == ELM_MWND_PRINT || x == ELM_MWND_SEL || x == ELM_MWND_KOE || x == ELM_MWND_SELMSG ||
           x == ELM_MWND_SEL_CANCEL || x == ELM_MWND_SELMSG_CANCEL;
}

static bool is_stage_root(int r) { return r == ELM_ROOT_BACK || r == ELM_ROOT_FRONT || r == ELM_ROOT_NEXT; }

static bool needs_inline(const int *ch, int n)
{
    if (n < 1) return false;
    int r = ch[0];
    switch (r) {
    case ELM_ROOT_PRINT: case ELM_ROOT_KOE: case ELM_ROOT_SEL: case ELM_ROOT_SELBTN: case ELM_ROOT_SELBTN_CANCEL:
    case ELM_ROOT_SELMSG: case ELM_ROOT_SEL_CANCEL: case ELM_ROOT_SELMSG_CANCEL: case ELM_ROOT_SELBTN_START:
        return true;
    default: break;
    }
    /* stage.mwndlist[i].<sub> and stage[s].mwndlist[i].<sub> (mwndlist = stage sub 3) */
    if (is_stage_root(r) && n >= 5 && ch[1] == ELM_STAGE_MWND && ch[2] == -1 && mwnd_sub(ch[4])) return true;
    return r == ELM_ROOT_STAGE && n >= 7 && ch[1] == -1 && ch[3] == ELM_STAGE_MWND && ch[4] == -1 && mwnd_sub(ch[6]);
}

/* ---- errors and halting --------------------------------------------------------------------------------------- */

void vm_err(const char *fmt, ...)
{
    char msg[VM_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    str_vfmt_cut(msg, sizeof msg, fmt, ap);  /* a longer message is cut, as the counters key on it */
    va_end(ap);
    g_vm.st.errs++;
    vm_ref_note_err(msg);
    log_msg(LOG_DEBUG, "vm: [%s:%d] %s", vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line, msg);
}

int vm_err_count(void) { return g_vm.st.errs; }

void vm_stop(const char *fmt, ...)
{
    if (g_vm.stop) return;
    va_list ap;
    va_start(ap, fmt);
    str_vfmt_cut(g_vm.reason, sizeof g_vm.reason, fmt, ap);   /* cut at a code point if longer */
    va_end(ap);
    g_vm.stop = true;
    g_vm.halt = VM_HALT_STOP;
    if (!elm_vm_stubs())
        log_msg(LOG_WARN, "vm: script stopped at %s:%d: %s", vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line,
                g_vm.reason);
}

void vm_fault(const char *type, const char *fmt, ...)
{
    if (g_vm.stop) return;
    char msg[VM_MSG_MAX];
    va_list ap;
    va_start(ap, fmt);
    str_vfmt_cut(msg, sizeof msg, fmt, ap);
    va_end(ap);
    const char *scene = vm_scene_name_utf8(g_vm.pos.scene);
    if (g_vm.pos.pc == 0)                         /* "%#x" prints 0 without the 0x: vm_ref.py says 0x0 */
        str_fmt_cut(g_vm.reason, sizeof g_vm.reason, "VM ERROR %s: %s at %s:0x0 line %d", type, msg, scene,
                    g_vm.pos.line);
    else
        str_fmt_cut(g_vm.reason, sizeof g_vm.reason, "VM ERROR %s: %s at %s:%#x line %d", type, msg, scene, g_vm.pos.pc,
                      g_vm.pos.line);
    g_vm.stop = true;
    g_vm.halt = VM_HALT_ERROR;
    log_msg(LOG_ERR, "vm: %s", g_vm.reason);
}

static VmHooks g_hooks;
static VmTextOps g_text_ops;

void vm_set_text_ops(const VmTextOps *ops)
{
    if (ops) g_text_ops = *ops;
    else memset(&g_text_ops, 0, sizeof g_text_ops);
}

void vm_set_hooks(const VmHooks *h)
{
    if (h) g_hooks = *h;
    else memset(&g_hooks, 0, sizeof g_hooks);
}

const VmHooks *vm_hooks(void) { return &g_hooks; }

bool vm_halted(void) { return g_vm.stop; }
const char *vm_halt_reason(void) { return g_vm.reason; }
VmHaltKind vm_halt_kind(void) { return g_vm.stop ? g_vm.halt : VM_HALT_NONE; }

void vm_clear_stop(void)
{
    g_vm.stop = false;
    g_vm.halt = VM_HALT_NONE;
    g_vm.reason[0] = 0;
}


/* ---- argument lists (FUN_005a8fd0) ---------------------------------------------------------------------------- */

/* Count + fresh args of one list; NULL (and *n = 0) after a fault. */
static Arg *arglist_head(int *n)
{
    *n = rd32();
    if (*n < 0 || *n > VM_ARGLIST_LEN_MAX) { vm_fault("VMError", "bad arglist count %d", *n); *n = 0; return NULL; }
    Arg *a = vm_args_alloc(*n);
    if (!a) *n = 0;
    return a;
}

static void read_value(Arg *a, int form)
{
    if (form == ELM_FORM_INT || form == ELM_FORM_LABEL) { a->form = ELM_FORM_INT; a->i = vm_pop_int(); }
    else if (form == ELM_FORM_STR) { a->form = ELM_FORM_STR; (void)vm_str_pop_swap(&a->s); }   /* underflow halts */
    else { a->form = form; a->nchain = vm_pop_chain(a->chain); }
}

/* Reads a list and its nested lists without recursion: the forms are stored last-first, and a nested list is read
 * completely (depth first) before the next argument of its parent, so frame st[d] is the list at nesting d. */
static int read_arglist(Arg **out)
{
    typedef struct { Arg *a; int k; } Level;
    Level st[VM_ARGLIST_DEPTH_MAX + 1];
    int n, depth = 0;
    *out = arglist_head(&n);
    if (!*out) return 0;
    st[0] = (Level){ *out, n - 1 };
    while (depth >= 0 && !g_vm.stop) {
        Level *lv = &st[depth];
        if (lv->k < 0) { depth--; continue; }
        Arg *a = &lv->a[lv->k--];
        int form = rd32();
        if (form != ELM_FORM_ARGLIST) { read_value(a, form); continue; }
        if (depth >= VM_ARGLIST_DEPTH_MAX) {
            vm_fault("VMError", "argument lists nested deeper than %d", depth);
            break;
        }
        ArgList *sub = vm_sublist_alloc();
        if (!sub) break;
        sub->v = arglist_head(&sub->n);
        a->form = ELM_FORM_ARGLIST;
        a->sub = sub;
        if (sub->v) st[++depth] = (Level){ sub->v, sub->n - 1 };
    }
    return n;
}

static void dispatch_traced(const int *ch, int n, ElmCall *c)
{
    if (c->op == OP_COMMAND && log_trace_on("CMD")) {
        SB_ON_STACK(line, VM_CMD_LINE_MAX);
        elm_fmt_call(&line, ch, n, c);
        if (g_vm.inline_pending) (void)sb_fmt(&line, " rf=%d", g_vm.inline_val);   /* a full line just stays cut */
        log_trace("CMD", "[%s:%d] %s", vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line, sb_cstr(&line));
    }
    elm_dispatch(ch, n, c);
}

/* ---- opcodes: stacks and declarations ------------------------------------------------------------------------- */

static void op_nl(uint32_t at)
{
    g_vm.pos.line = rd32();
    const Frame *f = vm_frame_top();
    VmDepths d = vm_depths();
    if (d.isp == f->base.isp && d.ssp == f->base.ssp && d.nmk == f->base.nmk) return;
    vm_ref_note_nl(at, d, f->base);
    g_vm.st.nl_viol++;
    log_msg(LOG_DEBUG, "vm: stack balance violation at %s:%d", vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line);
}

static void op_push(void)
{
    int32_t form = rd32(), v = rd32();
    if (g_vm.stop) return;
    if (form == ELM_FORM_INT) { vm_push_int(v); return; }
    if (form != ELM_FORM_STR) return;
    const Scene *sc = cur_sc();
    if (v < 0 || v >= sc->nstr) { vm_fault("IndexError", "string index %d out of range", v); return; }
    WStr *slot = vm_str_push_slot();
    if (slot) scene_str(sc, v, slot);
}

static void op_pop(void)
{
    int32_t form = rd32();
    if (form == ELM_FORM_INT) (void)vm_pop_int();       /* the value is discarded by definition */
    else if (form == ELM_FORM_STR) (void)vm_str_drop();
}

static void op_copy(void)
{
    int32_t form = rd32();
    if (form == ELM_FORM_INT) (void)vm_dup_int();       /* failures halt the script */
    else if (form == ELM_FORM_STR) (void)vm_dup_str();
}

static void op_dec_prop(void)
{
    int32_t form = rd32(), name = rd32();
    int size = (form == ELM_FORM_INTLIST || form == ELM_FORM_STRLIST) ? vm_pop_int() : 0;
    if (g_vm.stop) return;
    Prop *p = vm_frame_add_local(vm_frame_top());
    if (!p) { vm_fault("VMError", "more than %d call locals", VM_LOCAL_MAX); return; }
    vm_prop_reset(p, form, size, name);
}

static void op_arg(void)                       /* FUN_005a51b0: pop the declared locals (last first) */
{
    Frame *f = vm_frame_top();
    for (int i = f->nloc - 1; i >= 0 && !g_vm.stop; i--) {
        Prop *p = vm_frame_local(f, i);
        if (p->form == ELM_FORM_INT) p->i = vm_pop_int();
        else if (p->form == ELM_FORM_STR) (void)vm_str_pop_swap(&p->s);
        else p->nchain = vm_pop_chain(p->chain);
    }
    vm_frame_set_base(f);
}

/* ---- opcodes: control flow ------------------------------------------------------------------------------------ */

static void op_goto(uint8_t op)
{
    bool take = true;
    if (op != BC_GOTO) {
        int32_t v = vm_pop_int();
        take = op == BC_GOTO_TRUE ? v != 0 : v == 0;
    }
    int32_t lab = rd32();
    if (take && !g_vm.stop) jump_label(lab);
}

static void op_gosub(uint8_t op)               /* unused by the game */
{
    int32_t lab = rd32();
    Arg *a;
    int n = read_arglist(&a);
    if (!g_vm.stop) {
        Frame *f = vm_frame_push(VM_FRAME_GOSUB, op == BC_GOSUB ? ELM_FORM_INT : ELM_FORM_STR);
        if (f) {
            jump_label(lab);
            vm_frame_set_base(f);
            vm_frame_pass_args(f, a, n);
        }
    }
    vm_args_release();
}

static void push_return_value(int ret_form, const Arg *a, int n)
{
    if (ret_form == ELM_FORM_INT) vm_push_int(n == 1 && a[0].form == ELM_FORM_INT ? a[0].i : 0);
    else if (ret_form == ELM_FORM_STR) vm_push_str(n == 1 && a[0].form == ELM_FORM_STR ? &a[0].s : NULL);
}

static void op_return(void)                    /* FUN_005a4ab0 */
{
    Arg *a;
    int n = read_arglist(&a);
    if (g_vm.stop) { vm_args_release(); return; }
    if (vm_depth() == 1) { vm_args_release(); vm_stop("RETURN with empty call stack"); return; }
    vm_frame_set_depth(vm_depth() - 1);
    const Frame *f = vm_frame_top();
    vm_set_pos(f->rs);
    push_return_value(f->ret_form, a, n);
    vm_args_release();
}

/* ---- opcodes: elements ---------------------------------------------------------------------------------------- */

static void op_property(void)
{
    int ch[VM_CHAIN_MAX];
    int n = vm_pop_chain(ch);
    if (g_vm.stop) return;
    ElmCall c = { OP_PROPERTY, 0, NULL, 0, 0, 0, NULL };
    dispatch_traced(ch, n, &c);
}

static void op_assign(void)
{
    (void)rd32();                              /* form of the left side: informational */
    int32_t fr = rd32(), al = rd32();
    Arg *rhs = vm_rhs();
    if (fr == ELM_FORM_INT) rhs->i = vm_pop_int();
    else if (fr == ELM_FORM_STR) { rhs->form = ELM_FORM_STR; (void)vm_str_pop_swap(&rhs->s); }
    else { rhs->form = fr; rhs->nchain = vm_pop_chain(rhs->chain); }
    int ch[VM_CHAIN_MAX];
    int n = vm_pop_chain(ch);
    if (g_vm.stop) return;
    ElmCall c = { OP_ASSIGN, al, rhs, 1, 1, 0, NULL };
    dispatch_traced(ch, n, &c);
}

static void op_command(void)
{
    int32_t al = rd32();
    Arg *a;
    int n = read_arglist(&a);
    int ch[VM_CHAIN_MAX];
    int nch = vm_pop_chain(ch);
    int32_t nn = rd32();
    if (nn < 0 || nn > VM_NAMED_MAX) { vm_fault("VMError", "bad named count %d", nn); vm_args_release(); return; }
    for (int k = 0; k < nn; k++) {             /* named ids, last argument first */
        int32_t id = rd32();
        if (a && n - 1 - k >= 0) a[n - 1 - k].id = id;
    }
    int32_t rf = rd32();
    if (g_vm.stop) { vm_args_release(); return; }
    if (needs_inline(ch, nch)) { g_vm.inline_val = rd32(); g_vm.inline_pending = true; }   /* VM §9 */
    ElmCall c = { OP_COMMAND, al, a, n - nn < 0 ? 0 : n - nn, n, rf, NULL };
    dispatch_traced(ch, nch, &c);
    g_vm.inline_pending = false;
    vm_args_release();
}

/* ---- opcodes: text -------------------------------------------------------------------------------------------- */

/* The TEXT line of trace §3.2 (and of the vm_ref log in --vm-stubs mode). */
static void text_line(int32_t rf, const WStr *text)
{
    Scratch sc = mem_scratch_begin(NULL);
    const char *u = ws_utf8_arena(sc.arena, text, VM_UTF8_MAX);
    const char *sp = ws_utf8_arena(sc.arena, &g_vm.speaker, VM_UTF8_MAX);
    bool has = g_vm.speaker.len > 0;
    vm_ref_text(rf, sp, has, u);
    log_trace("TEXT", "[%s:%d rf%d] %s%s%s", vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line, rf, sp, has ? ": " : "",
              u);
    mem_scratch_end(&sc);
}

static void op_text(void)
{
    int32_t rf = rd32();
    g_vm.st.texts++;
    if (elm_vm_stubs()) {
        WStr s = { 0 };
        vm_pop_str(&s);
        text_line(rf, &s);
        ws_free(&s);
        g_vm.speaker.len = 0;
    } else {
        const WStr *top = vm_str_top();
        if (top && log_trace_on("TEXT")) text_line(rf, top);
        g_vm.speaker.len = 0;
        if (g_text_ops.text) g_text_ops.text(rf);   /* the text module pops the string */
        else (void)vm_str_drop();
    }
    if (g_hooks.on_text) g_hooks.on_text((int)g_vm.st.texts);
}

static void op_name(void)
{
    if (elm_vm_stubs()) { vm_pop_str(&g_vm.speaker); return; }
    const WStr *top = vm_str_top();
    if (top) ws_copy(&g_vm.speaker, top);         /* kept for the TEXT trace line */
    if (g_text_ops.name) g_text_ops.name();
    else (void)vm_str_drop();
}

/* ---- opcodes: operators (VM §5) ------------------------------------------------------------------------------- */

static int32_t sar(int32_t l, int s) { return l < 0 ? ~(int32_t)(~(uint32_t)l >> s) : (int32_t)((uint32_t)l >> s); }

/* FUN_005a3fa0 int x int; false for an unknown operator (both operands consumed, nothing pushed). */
static bool op2_int(int o, int32_t l, int32_t r, int32_t *res)
{
    uint32_t ul = (uint32_t)l, ur = (uint32_t)r;
    switch (o) {
    case 0x01: *res = (int32_t)(ul + ur); return true;
    case 0x02: *res = (int32_t)(ul - ur); return true;
    case 0x03: *res = (int32_t)(ul * ur); return true;
    case 0x04:
        if (r == 0) { vm_err("0 除算を行いました！'/'"); *res = 0; }
        else *res = (l == INT32_MIN && r == -1) ? INT32_MIN : l / r;
        return true;
    case 0x05:
        if (r == 0) { vm_err("0 除算を行いました！'%%'"); *res = 0; }
        else *res = r == -1 ? 0 : l % r;
        return true;
    case 0x10: *res = l == r; return true;
    case 0x11: *res = l != r; return true;
    case 0x12: *res = l > r; return true;
    case 0x13: *res = l >= r; return true;
    case 0x14: *res = l < r; return true;
    case 0x15: *res = l <= r; return true;
    case 0x20: *res = l != 0 && r != 0; return true;
    case 0x21: *res = l != 0 || r != 0; return true;
    case 0x31: *res = l & r; return true;
    case 0x32: *res = l | r; return true;
    case 0x33: *res = l ^ r; return true;
    case 0x34: *res = (int32_t)(ul << (r & 31)); return true;
    case 0x35: *res = sar(l, r & 31); return true;
    case 0x36: *res = (int32_t)(ul >> (r & 31)); return true;
    default: return false;
    }
}

static void op2_str_int(int o)                 /* FUN_005a4290: str * n repeats */
{
    int32_t n = vm_pop_int();
    WStr s = { 0 }, t = { 0 };
    vm_pop_str(&s);
    if (o == 0x03) {
        ws_set(&t, NULL, 0);
        for (int32_t k = 0; k < n && s.len > 0 && t.len < VM_STR_REPEAT_MAX; k++) ws_cat(&t, &s);
        vm_push_str(&t);
    }
    ws_free(&t);
    ws_free(&s);
}

static bool str_cmp_result(int o, int c)
{
    switch (o) {
    case 0x10: return c == 0;
    case 0x11: return c != 0;
    case 0x12: return c > 0;
    case 0x13: return c >= 0;
    case 0x14: return c < 0;
    default: return c <= 0;                    /* 0x15 */
    }
}

/* Operands of string OP2, popped by swapping buffers with the string stack: no allocation per operation. */
static WStr g_op_l, g_op_r;

static void op2_str_str(int o)
{
    if (!vm_str_pop_swap(&g_op_r) || !vm_str_pop_swap(&g_op_l)) return;   /* underflow: vm_fault was raised */
    if (o == 0x01) { ws_cat(&g_op_l, &g_op_r); vm_push_str(&g_op_l); }
    else if (o >= 0x10 && o <= 0x15) vm_push_int(str_cmp_result(o, ws_cmp_fold(&g_op_l, &g_op_r)));   /* FUN_005a4340 */
}

static void op_op1(void)                       /* FUN_005a3f60 */
{
    int32_t form = rd32();
    uint8_t o = rd8();
    if (form != ELM_FORM_INT || g_vm.stop) return;
    int32_t v = vm_pop_int();
    if (o == 0x01) vm_push_int(v);
    else if (o == 0x02) vm_push_int((int32_t)(0u - (uint32_t)v));
    else if (o == 0x30) vm_push_int(~v);
}

static void op_op2(void)
{
    int32_t fl = rd32(), fr = rd32();
    uint8_t o = rd8();
    if (g_vm.stop) return;
    if (fl == ELM_FORM_INT && fr == ELM_FORM_INT) {
        int32_t r = vm_pop_int(), l = vm_pop_int(), res;
        if (op2_int(o, l, r, &res)) vm_push_int(res);
    } else if (fl == ELM_FORM_STR && fr == ELM_FORM_INT) {
        op2_str_int(o);
    } else if (fl == ELM_FORM_STR && fr == ELM_FORM_STR) {
        op2_str_str(o);
    }
}

/* ---- the interpreter ------------------------------------------------------------------------------------------ */

void vm_step(void)
{
    g_vm.st.steps++;
    uint32_t at = g_vm.pos.pc;
    uint8_t op = rd8();
    if (g_vm.stop) return;
    switch (op) {
    case BC_NL:        op_nl(at); break;
    case BC_PUSH:      op_push(); break;
    case BC_POP:       op_pop(); break;
    case BC_COPY:      op_copy(); break;
    case BC_PROP:      op_property(); break;
    case BC_COPY_ELM:  (void)vm_copy_elm(); break;   /* failures halt the script */
    case BC_DEC_PROP:  op_dec_prop(); break;
    case BC_ELM_POINT: vm_mark_push(); break;
    case BC_ARG:       op_arg(); break;
    case BC_GOTO: case BC_GOTO_TRUE: case BC_GOTO_FALSE: op_goto(op); break;
    case BC_GOSUB: case BC_GOSUB_STR: op_gosub(op); break;
    case BC_RETURN:    op_return(); break;
    case BC_EOF:       vm_stop("EOF reached (ファイルの終端に到達しました)"); break;
    case BC_ASSIGN:    op_assign(); break;
    case BC_OP1:       op_op1(); break;
    case BC_OP2:       op_op2(); break;
    case BC_COMMAND:   op_command(); break;
    case BC_TEXT:      op_text(); break;
    case BC_NAME:      op_name(); break;
    case BC_END:       vm_stop("opcode 0 (スクリプトの解析に失敗しました)"); break;
    default:           break;                  /* 0x33/0x34 and unknown bytes: 1-byte no-op */
    }
}

static void halt_proc(void)
{
    Proc h = { .type = PROC_HALT };
    proc_push(&h);
}

void vm_run(void)
{
    if (!vm_pack()) return;
    int64_t guard = 0;
    while (!g_vm.stop && proc_top()->type == PROC_SCRIPT) {
        if (g_hooks.on_step && g_hooks.on_step()) return;   /* --steps / --until-text / quit: stay put */
        vm_step();
        if (++guard >= VM_RUN_GUARD) {         /* runaway loop without a yield: give the frame back */
            log_msg(LOG_WARN, "vm: %" PRId64 " instructions without a wait at %s:%d; yielding", guard,
                    vm_scene_name_utf8(g_vm.pos.scene), g_vm.pos.line);
            Proc y = { .type = PROC_YIELD };
            proc_push(&y);
            break;
        }
    }
    if (g_vm.stop && proc_top()->type == PROC_SCRIPT) {   /* EOF / fault: FUN_005a7350(0) */
        if (g_hooks.on_stop) g_hooks.on_stop(g_vm.reason);
        if (g_vm.halt == VM_HALT_ERROR && g_hooks.on_error) g_hooks.on_error(g_vm.reason);
        halt_proc();
    }
}

int64_t vm_run_steps(int64_t max)
{
    int64_t n = 0;
    if (!vm_pack()) return 0;
    while (n < max && !g_vm.stop && proc_top()->type == PROC_SCRIPT) { vm_step(); n++; }
    if (g_vm.stop && proc_top()->type == PROC_SCRIPT) halt_proc();
    return n;
}

/* ---- init / start / shutdown ---------------------------------------------------------------------------------- */

void vm_reset_stacks(void)
{
    vm_stacks_clear();
    vm_frames_reset();
}

static void reset_state(void)
{
    vm_stacks_clear();
    vm_args_release();
    WStr speaker = g_vm.speaker;                  /* the buffer is kept for reuse */
    memset(&g_vm, 0, sizeof g_vm);
    g_vm.speaker = speaker;
    g_vm.speaker.len = 0;
    vm_ref_reset();
}

bool vm_init(const Pack *pk)
{
    if (!pk || pk->nscene <= 0) { log_msg(LOG_ERR, "vm_init: no scenes"); return false; }
    reset_state();
    if (!vm_store_bind(pk)) return false;
    g_vm.pos.line = -1;
    sys_bind_pack(pk);
    proc_init();
    elm_flow_register();
    sys_cg_register();
    return true;
}

void vm_shutdown(void)
{
    vm_store_unbind();
    vm_stacks_shutdown();
    vm_ref_shutdown();
    ws_free(&g_vm.speaker);
    ws_free(&g_op_l);
    ws_free(&g_op_r);
    memset(&g_vm, 0, sizeof g_vm);
    sys_shutdown();
}

bool vm_start(int scene, int z)
{
    if (!vm_pack() || scene < 0 || scene >= vm_pack()->nscene) {
        log_msg(LOG_ERR, "vm_start: bad scene %d", scene);
        return false;
    }
    vm_reset_stacks();
    vm_clear_stop();
    g_vm.speaker.len = 0;
    if (!vm_goto_z(scene, z)) vm_jump(scene, 0);
    proc_reset();
    return true;
}

bool vm_restart_at(int scene, int z)
{
    if (!vm_pack() || scene < 0 || scene >= vm_pack()->nscene) return vm_start(scene, z);   /* logs, no reset */
    vm_local_banks_reset();
    return vm_start(scene, z);
}

void vm_restart_menu(void)
{
    WStr m = { 0 };
    int idx = -1;
    if (gx_str("MENU_SCENE", 0, &m)) idx = vm_find_scene(&m);
    if (idx < 0) idx = vm_find_scene_utf8("_menu");
    ws_free(&m);
    if (idx < 0) { log_msg(LOG_ERR, "vm: #MENU_SCENE not found"); idx = 0; }
    (void)vm_restart_at(idx, 0);   /* idx is a valid scene of the bound pack; without a pack it logs */
}
