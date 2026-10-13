/* ui/ui_elm.c — the script elements of the ui module: roots 46 mouse, 63 syscom, 86 input, 97 editbox, and the
 * system roots the game never uses (key, file, excall, ... answer as unknown). Semantics: SYS §3.2–3.5;
 * sub-ids: element_defs_profile0.tsv. Registered by ui_init (ENGINE_SPEC §1.1 root ownership). */
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "core/log.h"
#include "fmt/gameexe.h"
#include "gfx/canvas.h"
#include "plat/input.h"
#include "sav/sav.h"
#include "vm/elements.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include <SDL3/SDL_scancode.h>

enum { EDITBOX_MAX = 4, SUB_MAX = 64 };

typedef enum { STUB_INPUT, STUB_MOUSE, STUB_SYSCOM, STUB_GROUPS } StubGroup;

static void stub_sub(StubGroup g, int sub)     /* known-but-unused sub: log once per (group, sub) */
{
    static const char *const NAME[STUB_GROUPS] = { "input", "mouse", "syscom" };
    static bool seen[STUB_GROUPS][SUB_MAX];
    if (sub < 0 || sub >= SUB_MAX || seen[g][sub]) return;
    seen[g][sub] = true;
    log_msg(LOG_DEBUG, "ui: %s.%d unused, stubbed", NAME[g], sub);
}

/* Writes an int through an element-reference argument (form 13), as the VM's ASSIGN would (VM §4.1). */
static void assign_ref(const Arg *a, int32_t v)
{
    if (!a || a->nchain <= 0) return;
    Arg rhs = { .id = -1, .form = ELM_FORM_INT, .i = v };
    ElmCall k = { .op = OP_ASSIGN, .al_id = 1, .arg = &rhs, .npos = 1, .nall = 1 };
    elm_dispatch(a->chain, a->nchain, &k);
}

/* A script-opened modal dialog pauses the script: yield so vm_run returns; the main loop then runs ui_frame
 * until the dialog closes (the original's modal Win32 loop). */
static void yield_for_ui(void)
{
    if (!ui_active()) return;
    Proc p = { .type = PROC_YIELD };
    proc_push(&p);
}

/* ---- 86 input (FUN_0065b1b0) ---------------------------------------------------------------------------------- */
static ElmResult elm_input(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 1) return ELM_UNKNOWN;
    int code = nrest >= 2 ? rest[1] : -1;
    switch (rest[0]) {
    case ELM_INPUT_DECIDE: elm_ret_int(c, input_query(VK_DECIDE, code)); return ELM_OK; /* decide.<code> */
    case ELM_INPUT_CANCEL: elm_ret_int(c, input_query(VK_CANCEL, code)); return ELM_OK; /* cancel.<code> */
    case ELM_INPUT_CLEAR:
        input_clear();
        elm_ret_none(c);
        return ELM_OK;
    case ELM_INPUT_NEXT:
        input_next();
        elm_ret_none(c);
        return ELM_OK;
    case ELM_INPUT_WAIT:
    case ELM_INPUT_WAIT_FORCE:
        stub_sub(STUB_INPUT, rest[0]);
        elm_ret_none(c);
        return ELM_OK; /* wait / wait_force: unused */
    case ELM_INPUT_UP: elm_ret_int(c, input_query_key(SDL_SCANCODE_UP, code)); return ELM_OK;
    case ELM_INPUT_DOWN: elm_ret_int(c, input_query_key(SDL_SCANCODE_DOWN, code)); return ELM_OK;
    case ELM_INPUT_LEFT: elm_ret_int(c, input_query_key(SDL_SCANCODE_LEFT, code)); return ELM_OK;
    case ELM_INPUT_RIGHT: elm_ret_int(c, input_query_key(SDL_SCANCODE_RIGHT, code)); return ELM_OK;
    case ELM_INPUT_GET_LAST_DEVICE_TYPE: elm_ret_int(c, 0); return ELM_OK; /* last device = mouse/keyboard */
    default: return ELM_UNKNOWN;
    }
}

/* ---- 46 mouse (FUN_0065b2a0) ---------------------------------------------------------------------------------- */
static ElmResult elm_mouse(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 1) return ELM_UNKNOWN;
    int code = nrest >= 2 ? rest[1] : -1;
    switch (rest[0]) {
    case ELM_MOUSE_POS_X:
    case ELM_MOUSE_GET_POS_X: elm_ret_int(c, input_mouse_x()); return ELM_OK;
    case ELM_MOUSE_POS_Y:
    case ELM_MOUSE_GET_POS_Y: elm_ret_int(c, input_mouse_y()); return ELM_OK;
    case ELM_MOUSE_CLEAR:
        input_mouse_clear();
        elm_ret_none(c);
        return ELM_OK;
    case ELM_MOUSE_WHEEL: elm_ret_int(c, input_wheel()); return ELM_OK;
    case ELM_MOUSE_RIGHT: elm_ret_int(c, input_query_mouse(1, code)); return ELM_OK;
    case ELM_MOUSE_LEFT: elm_ret_int(c, input_query_mouse(0, code)); return ELM_OK;
    case ELM_MOUSE_NEXT:
        input_next();
        elm_ret_none(c);
        return ELM_OK;
    case ELM_MOUSE_GET_POS: /* get_pos(&x, &y): _menu hover */
        if (c->npos >= 1 && c->arg) assign_ref(&c->arg[0], input_mouse_x());
        if (c->npos >= 2 && c->arg) assign_ref(&c->arg[1], input_mouse_y());
        elm_ret_none(c);
        return ELM_OK;
    case ELM_MOUSE_SET_POS:
        stub_sub(STUB_MOUSE, 10);
        elm_ret_none(c);
        return ELM_OK; /* set_pos: unused */
    default: return ELM_UNKNOWN;
    }
}

/* ---- 63 syscom (FUN_00669570) --------------------------------------------------------------------------------- */

/* The config value of a set/set_default/get syscom triple, its default, and which of the three sub is. */
typedef enum { CFG_SET, CFG_SET_DEFAULT, CFG_GET } CfgOp;

static int *cfg_int(int sub, int *def, CfgOp *op)
{
    static const struct { const char *key; int first, def; } T[] = {
        { "CONFIG.VOLUME.BGM", 21, 255 }, { "CONFIG.VOLUME.KOE", 26, 255 }, { "CONFIG.VOLUME.PCM", 29, 255 },
        { "CONFIG.VOLUME.SE", 32, 255 }, { "CONFIG.VOLUME.ALL", 39, 255 }, { "CONFIG.MESSAGE_SPEED", 46, 20 },
        { NULL, 51, SAV_AUTO_MOJI_WAIT_DEFAULT }, { NULL, 54, SAV_AUTO_MIN_WAIT_DEFAULT },
    };
    SavConfig *k = sav_config();
    int *field[COUNT_OF(T)] = { &k->audio.vol_bgm, &k->audio.vol_koe, &k->audio.vol_pcm, &k->audio.vol_se,
                                &k->audio.vol_all, &k->msg_speed, &k->auto_moji_wait, &k->auto_min_wait };
    for (size_t i = 0; i < COUNT_OF(T); i++) {
        /* BGM is 21/24/25; the others are first/first+1/first+2 (set / set_default / get). */
        int set = T[i].first, setdef = i == 0 ? 24 : set + 1, get = i == 0 ? 25 : set + 2;
        if (sub != set && sub != setdef && sub != get) continue;
        *op = sub == set ? CFG_SET : sub == setdef ? CFG_SET_DEFAULT : CFG_GET;
        *def = T[i].key ? gx_int(T[i].key, 0, T[i].def) : T[i].def;
        return field[i];
    }
    return NULL;
}

static ElmResult syscom_config(int sub, ElmCall *c)
{
    int def;
    CfgOp op;
    int *v = cfg_int(sub, &def, &op);
    if (v) {
        if (op == CFG_GET) { elm_ret_int(c, *v); return ELM_OK; }
        *v = op == CFG_SET ? elm_arg_int(c, 0, def) : def;
        ui_config_apply();
        elm_ret_none(c);
        return ELM_OK;
    }
    if (sub >= SUB_MAX) return ELM_UNKNOWN;
    stub_sub(STUB_SYSCOM, sub);                /* ignores negative subs */
    if (c->ret_form == ELM_FORM_INT || c->op == OP_PROPERTY) elm_ret_int(c, 0);
    else elm_ret_none(c);
    return ELM_OK;
}

static ElmResult elm_syscom(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 1) return ELM_UNKNOWN;
    int sub = rest[0];
    switch (sub) {
    case ELM_SYSCOM_CALL_SYSCOM_MENU: ui_open_syscom(); break;
    case ELM_SYSCOM_CALL_SAVE_MENU: ui_open_save(); break;
    case ELM_SYSCOM_CALL_LOAD_MENU: ui_open_load(); break;     /* _menu "ロード" */
    case ELM_SYSCOM_CALL_CONFIG_MENU: ui_open_config(); break; /* _menu "環境設定" */
    case ELM_SYSCOM_SET_WINDOW_MODE:
        sav_config()->window_mode = elm_arg_int(c, 0, 0) != 0 ? 1 : 0;
        ui_config_apply();
        elm_ret_none(c);
        return ELM_OK;
    case ELM_SYSCOM_INIT_SYSCOM_FLAG: elm_ret_none(c); return ELM_OK;
    case ELM_SYSCOM_SET_SYSCOM_MENU_ENABLE:
        sys_state()->syscom_locked = false;
        elm_ret_none(c);
        return ELM_OK; /* L+0x334 = 0 */
    case ELM_SYSCOM_SET_SYSCOM_MENU_DISABLE:
        sys_state()->syscom_locked = true;
        elm_ret_none(c);
        return ELM_OK; /* L+0x334 = 1 */
    case ELM_SYSCOM_GET_WINDOW_MODE: elm_ret_int(c, sav_config()->window_mode == 1); return ELM_OK;
    case ELM_SYSCOM_GET_SAVELOAD_ALERT_ONOFF:
    case ELM_SYSCOM_GET_BGM_ONOFF:
    case ELM_SYSCOM_GET_KOE_ONOFF:
    case ELM_SYSCOM_GET_PCM_ONOFF:
    case ELM_SYSCOM_GET_SE_ONOFF: elm_ret_int(c, 1); return ELM_OK; /* get_saveload_alert / get_*_onoff */
    default: return syscom_config(sub, c);
    }
    yield_for_ui();                            /* 0-3 opened a modal dialog */
    elm_ret_none(c);
    return ELM_OK;
}

/* ---- 97 editbox[4] (FUN_0065ba70) ----------------------------------------------------------------------------- */
static void editbox_text(int id, const ElmCall *c)
{
    WStr s = { 0 };
    ui_editbox_get_text(id, &s);
    elm_ret_str(c, &s);
    ws_free(&s);
}

static ElmResult elm_editbox(const int *rest, int nrest, ElmCall *c)
{
    if (nrest < 3 || rest[0] != -1) return ELM_UNKNOWN;
    int id = rest[1];
    if (id < 0 || id >= gx_int("EDITBOX.CNT", 0, EDITBOX_MAX) || id >= EDITBOX_MAX) {
        log_msg(LOG_WARN, "ui: editbox[%d] out of range", id);
        elm_ret_none(c);
        return ELM_OK;
    }
    switch (rest[2]) {
    case ELM_EDITBOX_CREATE: { /* create(x, y, w, h, moji_size) */
        Rect r = rect_at(elm_arg_int(c, 0, 0), elm_arg_int(c, 1, 0), elm_arg_int(c, 2, 0), elm_arg_int(c, 3, 0));
        ui_editbox_create(id, r, elm_arg_int(c, 4, 24));
        break;
    }
    case ELM_EDITBOX_DESTROY: ui_editbox_destroy(id); break;
    case ELM_EDITBOX_SET_TEXT: ui_editbox_set_text(id, elm_arg_str(c, 0)); break;
    case ELM_EDITBOX_GET_TEXT: editbox_text(id, c); return ELM_OK;
    case ELM_EDITBOX_CHECK_DECIDED: elm_ret_int(c, ui_editbox_decided(id)); return ELM_OK;
    case ELM_EDITBOX_CHECK_CANCELED: elm_ret_int(c, ui_editbox_canceled(id)); return ELM_OK;
    case ELM_EDITBOX_SET_FOCUS: ui_editbox_set_focus(id); break;
    case ELM_EDITBOX_CLEAR_INPUT: ui_editbox_clear_input(id); break;
    default: return ELM_UNKNOWN;
    }
    elm_ret_none(c);
    return ELM_OK;
}

static ElmResult ui_root(int root, const int *rest, int nrest, ElmCall *c)
{
    switch (root) {
    case ELM_ROOT_MOUSE: return elm_mouse(rest, nrest, c);
    case ELM_ROOT_SYSCOM: return elm_syscom(rest, nrest, c);
    case ELM_ROOT_INPUT: return elm_input(rest, nrest, c);
    case ELM_ROOT_EDITBOX: return elm_editbox(rest, nrest, c);
    default: return ELM_UNKNOWN;
    }
}

/* The system roots (ENGINE_SPEC §1.1); 166 steam and 167 keyboard lie beyond the dispatch table and stay unknown. */
void ui_elm_register(void)
{
    static const int ROOTS[] = {
        ELM_ROOT_KEY, ELM_ROOT_MOUSE, ELM_ROOT_FILE, ELM_ROOT_FRAME_ACTION_CH, ELM_ROOT_SYSCOM, ELM_ROOT_EXCALL,
        ELM_ROOT_SYSCOM_MENU, ELM_ROOT_MWND_BTN, ELM_ROOT_FRAME_ACTION, ELM_ROOT_INPUT, ELM_ROOT_SYSTEM,
        ELM_ROOT_EDITBOX, ELM_ROOT_DATABASE,
    };
    elm_register_roots(ROOTS, (int)COUNT_OF(ROOTS), ui_root);
}
