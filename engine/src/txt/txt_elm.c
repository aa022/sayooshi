/* txt/txt_elm.c — global text roots (10, 11, 14, 15, 16, 18, 22, 84, 100, 115 + the other mwnd forwards) and the
 * TEXT/NAME opcodes (TXT §2–§4, ENGINE_SPEC §1.1). Every forward runs one mwnd sub-command on the current window. */
#include "vm/elements.h"
#include "core/str.h"
#include "txt/mwnd.h"
#include "txt/txt_internal.h"
#include "vm/vm.h"

/* Global root → mwnd sub-code (dispatcher FUN_00659170; table in VM §9). */
static int root_sub(int root)
{
    switch (root) {
    case ELM_ROOT_OPEN: return 1;
    case ELM_ROOT_CLOSE: return 2;
    case ELM_ROOT_CLEAR: return 3;
    case ELM_ROOT_PRINT: return 4;
    case ELM_ROOT_PP: return 0x13;
    case ELM_ROOT_R: return 0x14;
    case ELM_ROOT_NL: return 6;
    case ELM_ROOT_SIZE: return 7;
    case ELM_ROOT_COLOR: return 8;
    case ELM_ROOT_KOE: return 9;
    case ELM_ROOT_SEL: return 5;
    case ELM_ROOT_WAIT_MSG: return 0x12;
    case ELM_ROOT_SET_WAKU: return 0;
    case ELM_ROOT_SET_FACE: return 0x15;
    case ELM_ROOT_CLOSE_WAIT: return 0xd;
    case ELM_ROOT_CLOSE_NOWAIT: return 0xe;
    case ELM_ROOT_OPEN_WAIT: return 0xf;
    case ELM_ROOT_OPEN_NOWAIT: return 0x10;
    case ELM_ROOT_RUBY: return 0xc;
    case ELM_ROOT_NLI: return 0x11;
    case ELM_ROOT_CLEAR_FACE: return 0x16;
    case ELM_ROOT_MSG_BLOCK: return 0x31;
    case ELM_ROOT_KOE_PLAY_WAIT: return 0x1a;
    case ELM_ROOT_KOE_PLAY_WAIT_KEY: return 0x1b;
    case ELM_ROOT_NEXT_MSG: return 0x1d;
    case ELM_ROOT_CLEAR_INDENT: return 0x1c;
    case ELM_ROOT_MULTI_MSG: return 0x1f;
    case ELM_ROOT_SELMSG: return 0x32;
    case ELM_ROOT_SEL_CANCEL: return 0x33;
    case ELM_ROOT_SELMSG_CANCEL: return 0x34;
    case ELM_ROOT_PAGE: return 0x36;
    case ELM_ROOT_GET_SEL_MWND: return 0;
    case ELM_ROOT_INDENT: return 0x38;
    case ELM_ROOT_START_SLIDE_MSG: return 0x3a;
    case ELM_ROOT_MSG_PP_BLOCK: return 0x3b;
    case ELM_ROOT_END_SLIDE_MSG: return 0x3c;
    case ELM_ROOT_END_CLOSE: return 0x40;
    case ELM_ROOT_REP_POS: return 0x54;
    case ELM_ROOT_SET_NAMAE: return 0x55;
    default: return -1;
    }
}

static ElmResult txt_elm_global(int root, const int *rest, int nrest, ElmCall *c)
{
    if (root == MSG_DEFER_ROOT && nrest == 2) {          /* deferred sub-command from proc 0x12 (mwnd.h) */
        mwnd_sub(mwnd_get(rest[1]), rest[0], c);
        return ELM_OK;
    }
    int sub = root_sub(root);
    if (sub < 0 || nrest != 0) return ELM_UNKNOWN;
    mwnd_sub(mwnd_cur(), sub, c);
    return ELM_OK;
}

/* `S.3.-1.n.<sub>` chains: rest = [n, sub]. All stages share the same window objects in this port. */
ElmResult txt_elm_mwnd(int stage, const int *rest, int nrest, ElmCall *c)
{
    (void)stage;
    if (nrest == 1 && c && c->op == OP_PROPERTY) { elm_push_default(c); return ELM_OK; }   /* the window itself */
    if (nrest != 2 || rest[0] < 0 || rest[0] >= MWND_CNT) return ELM_UNKNOWN;
    mwnd_sub(mwnd_get(rest[0]), rest[1], c);
    return ELM_OK;
}

/* TEXT / NAME pop their string by swapping buffers with the VM's string stack: no allocation per line. */
static WStr g_popped;

static void op_text(int read_flag)
{
    if (vm_str_pop_swap(&g_popped)) mwnd_print_text(&g_popped, vm_scene(), read_flag);
}

static void op_name(void)
{
    if (vm_str_pop_swap(&g_popped)) mwnd_set_name(&g_popped);
}

void txt_elm_shutdown(void) { ws_free(&g_popped); }

/* Claims the text roots (ENGINE_SPEC §1.1) and the TEXT / NAME opcodes (mwnd_init). */
void txt_elm_register(void)
{
    static const int ROOTS[] = {
        ELM_ROOT_SET_MWND, ELM_ROOT_OPEN, ELM_ROOT_CLOSE, ELM_ROOT_CLEAR, ELM_ROOT_PRINT, ELM_ROOT_PP, ELM_ROOT_R,
        ELM_ROOT_NL, ELM_ROOT_SIZE, ELM_ROOT_COLOR, ELM_ROOT_KOE, ELM_ROOT_SEL, ELM_ROOT_WAIT_MSG, ELM_ROOT_SET_WAKU,
        ELM_ROOT_SET_FACE, ELM_ROOT_MSGBTN, ELM_ROOT_CLOSE_WAIT, ELM_ROOT_CLOSE_NOWAIT, ELM_ROOT_OPEN_WAIT,
        ELM_ROOT_OPEN_NOWAIT, ELM_ROOT_NOP, ELM_ROOT_RUBY, ELM_ROOT_NLI, ELM_ROOT_SET_SEL_MWND, ELM_ROOT_CLEAR_FACE,
        ELM_ROOT_SELBTN, ELM_ROOT_SELBTN_CANCEL, ELM_ROOT_MSG_BLOCK, ELM_ROOT_KOE_PLAY_WAIT, ELM_ROOT_KOE_PLAY_WAIT_KEY,
        ELM_ROOT_NEXT_MSG, ELM_ROOT_CLEAR_INDENT, ELM_ROOT_MULTI_MSG, ELM_ROOT_SELMSG, ELM_ROOT_SEL_CANCEL,
        ELM_ROOT_SELMSG_CANCEL, ELM_ROOT_INSERT_MSGBK_IMG, ELM_ROOT_PAGE, ELM_ROOT_GET_MWND, ELM_ROOT_GET_SEL_MWND,
        ELM_ROOT_CLEAR_MSGBK, ELM_ROOT_INDENT, ELM_ROOT_START_SLIDE_MSG, ELM_ROOT_MSG_PP_BLOCK, ELM_ROOT_END_SLIDE_MSG,
        ELM_ROOT_END_CLOSE, ELM_ROOT_SELBTN_READY, ELM_ROOT_SELBTN_START, ELM_ROOT_SELBTN_CANCEL_READY, ELM_ROOT_MSGBK,
        ELM_ROOT_REP_POS, ELM_ROOT_SET_NAMAE, ELM_ROOT_SEL_IMAGE, ELM_ROOT_GET_LAST_SEL_MSG,
    };
    static const VmTextOps OPS = { op_text, op_name };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), txt_elm_global);
    vm_set_text_ops(&OPS);
}
