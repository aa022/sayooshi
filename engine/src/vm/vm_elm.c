/* vm/vm_elm.c — the VM-level element roots (VM §4.3): banks 25-35/106/107/137, 83 call locals, 4 jump, 5 farcall,
 * 108 namae, 131/158/138-141 call-stack queries, and the user props (0x7F, VM §6.5) and user commands (0x7E, §7.3).
 * Every index that comes from the script is checked; a bad one is a vm_err (logged, counted) or a vm_fault. */
#include "vm/vm_internal.h"
#include "core/log.h"
#include <string.h>

enum { NAMAE_LOCAL_ROOT = 106, NAMAE_GLOBAL_ROOT = 107, CALL_LOCAL_TAG = 0x7D, REPR_MAX = 512, ARGS_MAX = 1024 };

/* ---- property records (FUN_005a4da0) -------------------------------------------------------------------------- */

static ElmResult h_prop(Prop *p, const int *rest, int nrest, ElmCall *c)
{
    switch (p->form) {
    case ELM_FORM_INT:
        if (c->al_id == 0) vm_push_int(p->i);
        else if (c->al_id == 1 && c->arg && c->nall > 0) p->i = c->arg[0].i;
        return ELM_OK;
    case ELM_FORM_STR: return elm_str(&p->s, rest, nrest, c);
    case ELM_FORM_INTLIST: return elm_intlist_obj(&p->il, rest, nrest, c);
    case ELM_FORM_STRLIST: return elm_strlist_obj(&p->sl, rest, nrest, c);
    case ELM_FORM_INTREF:
    case ELM_FORM_STRREF:
    case ELM_FORM_INTLISTREF:
    case ELM_FORM_STRLISTREF:
        vm_push_ref(p->chain, p->nchain);      /* the script appends the rest and re-dispatches */
        return ELM_OK;
    default:
        if (nrest == 0 && c->al_id == 1 && c->arg && c->nall > 0) {   /* rebind an element ref */
            p->nchain = core_clamp_i32(c->arg[0].nchain, 0, VM_CHAIN_MAX);
            memcpy(p->chain, c->arg[0].chain, (size_t)p->nchain * sizeof(int));
            return ELM_OK;
        }
        vm_push_ref(p->chain, p->nchain);
        return ELM_OK;
    }
}

ElmResult vm_user_prop(int i, const int *rest, int nrest, ElmCall *c)   /* FUN_005a4ce0 */
{
    Prop *p = vm_inc_prop(i);
    int k = i - vm_inc_prop_count(), scene = vm_scene();
    if (!p && k >= 0 && k < vm_scene_prop_count(scene)) p = vm_scene_prop(scene, k);
    if (!p) { vm_fault("IndexError", "user prop %d out of range", i); return ELM_OK; }
    return h_prop(p, rest, nrest, c);
}

/* Code position of user command i: an include command (any scene) or a command of the current scene. */
static bool user_cmd_target(int i, int *scene, uint32_t *pc)
{
    const Pack *pk = vm_pack();
    if (i >= 0 && i < pk->ninc_cmd) {
        *scene = pk->inc_cmd[i].scene;
        *pc = (uint32_t)pk->inc_cmd[i].ofs;
    } else {
        const Scene *sc = &pk->scene[vm_scene()];
        int k = i - pk->ninc_cmd;
        if (k < 0 || k >= sc->nscn_cmd || !sc->scn_cmd) {
            vm_fault("IndexError", "user command %d out of range", i);
            return false;
        }
        *scene = vm_scene();
        *pc = (uint32_t)vm_tab(sc->scn_cmd, k);
    }
    if (*scene < 0 || *scene >= pk->nscene) {
        vm_fault("IndexError", "user command %d: bad scene %d", i, *scene);
        return false;
    }
    return true;
}

ElmResult vm_user_cmd(int i, ElmCall *c)       /* FUN_005a4bd0 + FUN_0054c870 */
{
    int scene;
    uint32_t pc;
    if (!vm_pack() || !user_cmd_target(i, &scene, &pc)) return ELM_OK;
    vm_ref_count(VM_REF_USERCMD);
    Frame *f = vm_frame_push(VM_FRAME_USERCMD, c->ret_form);
    if (!f) return ELM_OK;
    vm_jump(scene, pc);
    vm_frame_set_base(f);
    for (int k = 0; k < c->npos && c->arg; k++) {   /* positional args only; named args are not passed */
        const Arg *a = &c->arg[k];
        if (a->form == ELM_FORM_INT) vm_push_int(a->i);
        else if (a->form == ELM_FORM_STR) vm_push_str(&a->s);
        else vm_push_ref(a->chain, a->nchain);
    }
    return ELM_OK;
}

/* ---- 83 call locals (FUN_00657530) ---------------------------------------------------------------------------- */

static ElmResult h_call(const int *rest, int nrest, ElmCall *c)
{
    Frame *f = vm_frame_top();
    if (nrest <= 0) { vm_err("call element used as value"); return ELM_OK; }
    int s = rest[0];
    if (s == 0) return elm_intlist_obj(&f->L, rest + 1, nrest - 1, c);
    if (s == 1) return elm_strlist_obj(&f->K, rest + 1, nrest - 1, c);
    if (((uint32_t)s >> 24) == CALL_LOCAL_TAG) {
        int i = s & 0xFFFF;
        if (i >= f->nloc) { vm_fault("VMError", "call-local %d not declared (have %d)", i, f->nloc); return ELM_OK; }
        return h_prop(vm_frame_local(f, i), rest + 1, nrest - 1, c);
    }
    vm_err("call: bad sub-element %d", s);
    return ELM_OK;
}

/* ---- 108 namae (SYS §4) --------------------------------------------------------------------------------------- */

static int namae_letter(uint16_t ch)
{
    if (ch >= 0xFF21 && ch <= 0xFF3A) return ch - 0xFF21;   /* Ａ..Ｚ */
    if (ch >= 'A' && ch <= 'Z') return ch - 'A';
    return -1;
}

/* FUN_005b67e0: "＊X"/"＊XY" -> (107, idx), "％X…" -> (106, idx); idx = l1 or (l1+1)*26 + l2. */
static bool namae_key(const WStr *k, int *bank, int *idx)
{
    if (k->len < 2 || k->len > 3) return false;
    uint16_t m = k->p[0];
    if (m == 0xFF0A || m == '*') *bank = NAMAE_GLOBAL_ROOT;
    else if (m == 0xFF05 || m == '%') *bank = NAMAE_LOCAL_ROOT;
    else return false;
    int a = namae_letter(k->p[1]);
    if (a < 0) return false;
    if (k->len == 2) { *idx = a; return true; }
    int b = namae_letter(k->p[2]);
    if (b < 0) return false;
    *idx = (a + 1) * 26 + b;
    return true;
}

static void namae_name(StrBuf *b, int bank, int idx)   /* the inverse, for the vm_ref log */
{
    (void)sb_put_cp(b, bank == NAMAE_GLOBAL_ROOT ? 0xFF0A : 0xFF05);   /* names are short: never truncated */
    if (idx >= 26) (void)sb_put_cp(b, (uint32_t)(0xFF21 + idx / 26 - 1));
    (void)sb_put_cp(b, (uint32_t)(0xFF21 + idx % 26));
}

static void namae_ref(const ElmCall *c)        /* namae(key) -> str-ref chain (ret_form 23) */
{
    int bank = NAMAE_GLOBAL_ROOT, idx = -1;
    const WStr *key = elm_arg_str(c, 0);
    if (!namae_key(key, &bank, &idx)) {
        Scratch sc = mem_scratch_begin(NULL);
        vm_err("namae: bad key '%s'", ws_utf8_arena(sc.arena, key, VM_UTF8_MAX));
        mem_scratch_end(&sc);
    }
    int ch[3] = { bank, -1, idx };
    vm_push_ref(ch, 3);
}

static void log_namae_assign(int root, int b, const int *rest, int nrest, const ElmCall *c)
{
    if (!vm_ref_logging() || (root != NAMAE_LOCAL_ROOT && root != NAMAE_GLOBAL_ROOT) || c->op != OP_ASSIGN ||
        nrest != 2 || rest[0] != -1 || rest[1] < 0 || rest[1] >= vm_str_bank(b)->n || !c->arg ||
        c->arg[0].form != ELM_FORM_STR)
        return;
    SB_ON_STACK(key, 32);
    SB_ON_STACK(val, REPR_MAX);
    namae_name(&key, root, rest[1]);
    elm_repr_wstr(&val, &c->arg[0].s);
    vm_ref_log("namae[%s] = %s", sb_cstr(&key), sb_cstr(&val));
}

/* ---- 4 jump / 5 farcall --------------------------------------------------------------------------------------- */

static int find_scene_or_stop(const WStr *scene)
{
    int idx = vm_find_scene(scene);
    if (idx < 0) {
        Scratch sc = mem_scratch_begin(NULL);
        vm_stop("scene not found: %s", ws_utf8_arena(sc.arena, scene, VM_UTF8_MAX));
        mem_scratch_end(&sc);
    }
    return idx;
}

static void elm_jump(const ElmCall *c)         /* jump(scene[, z]): FUN_0059bbe0 */
{
    int32_t z = c->al_id == 1 ? elm_arg_int(c, 1, 0) : 0;
    if (vm_ref_logging()) {
        SB_ON_STACK(nm, 256);
        elm_repr_wstr(&nm, elm_arg_str(c, 0));
        vm_ref_log("jump %s z%d", sb_cstr(&nm), z);
    }
    vm_ref_count(VM_REF_JUMP);
    int idx = find_scene_or_stop(elm_arg_str(c, 0));
    if (idx >= 0) (void)vm_goto_z(idx, z);    /* z >= 1000: stays where it is, as the original */
}

static void elm_farcall(const ElmCall *c)      /* farcall(scene[, z, extra...]): FUN_005a47b0 */
{
    int32_t z = c->al_id == 1 ? elm_arg_int(c, 1, 0) : 0;
    vm_ref_count(VM_REF_FARCALL);
    if (vm_ref_logging()) {
        SB_ON_STACK(nm, 256);
        SB_ON_STACK(ex, ARGS_MAX);
        elm_repr_wstr(&nm, elm_arg_str(c, 0));
        int skip = c->nall > 2 ? 2 : c->nall;
        elm_fmt_args(&ex, c->arg ? c->arg + skip : NULL, c->arg ? c->nall - skip : 0);
        vm_ref_log("farcall %s z%d args=%s", sb_cstr(&nm), z, sb_cstr(&ex));
    }
    int idx = find_scene_or_stop(elm_arg_str(c, 0));
    if (idx < 0) return;
    Frame *f = vm_frame_push(VM_FRAME_FARCALL, ELM_FORM_INT);   /* farcall always expects an int back */
    if (!f) return;
    (void)vm_goto_z(idx, z);
    vm_frame_set_base(f);
    if (c->al_id == 1 && c->arg && c->npos > 2) vm_frame_pass_args(f, c->arg + 2, c->npos - 2);
}

/* ---- the roots ------------------------------------------------------------------------------------------------ */

static void set_depth_relative(int32_t n)      /* 138: FUN_0059c270; a negative n never deepens the stack */
{
    int64_t d = (int64_t)vm_depth() - n;
    vm_frame_set_depth(d < 1 ? 1 : d > vm_depth() ? vm_depth() : (int)d);
}

static ElmResult call_stack_root(int root, ElmCall *c)
{
    switch (root) {
    case ELM_ROOT_GET_SCENE_NAME: vm_push_str(pack_scene_name(vm_pack(), vm_scene())); return ELM_OK;
    case ELM_ROOT_GET_LINE_NO: vm_push_int(vm_line()); return ELM_OK;
    case ELM_ROOT_GET_CALL_STACK_CNT: vm_push_int(vm_depth()); return ELM_OK;
    case ELM_ROOT_DEL_CALL_STACK: set_depth_relative(elm_arg_int(c, 0, 1)); return ELM_OK;
    case ELM_ROOT_SET_CALL_STACK_CNT: {
        int32_t n = elm_arg_int(c, 0, 0);
        if (n >= 1 && n < vm_depth()) vm_frame_set_depth(n);
        return ELM_OK;
    }
    case 141: vm_frame_set_depth(1); return ELM_OK;
    default: return ELM_UNKNOWN;
    }
}

ElmResult vm_elm_global(int root, const int *rest, int nrest, ElmCall *c)
{
    int b = vm_int_bank_index(root);
    if (b >= 0) return elm_intlist_obj(vm_int_bank(b), rest, nrest, c);
    b = vm_str_bank_index(root);
    if (b >= 0) {
        ElmResult r = elm_strlist_obj(vm_str_bank(b), rest, nrest, c);
        log_namae_assign(root, b, rest, nrest, c);
        return r;
    }
    switch (root) {
    case ELM_ROOT_CUR_CALL: return h_call(rest, nrest, c);
    case ELM_ROOT_JUMP: elm_jump(c); return ELM_OK;
    case ELM_ROOT_FARCALL: elm_farcall(c); return ELM_OK;
    case ELM_ROOT_RETURNMENU: /* only reached here in --vm-stubs mode */
        if (c->al_id == 0) vm_stub_restart();
        return ELM_OK;
    case ELM_ROOT_NAMAE: namae_ref(c); return ELM_OK;
    default: return call_stack_root(root, c);
    }
}
