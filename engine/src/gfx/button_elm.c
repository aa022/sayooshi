/* gfx/button_elm.c — the button elements: stage.6 objbtngroup[16] (FUN_00667e80, ENGINE_SPEC §1.4.4) and the
 * button sub-commands of an object (obj.42 set_button, FUN_005fb3b0, ...). gfx_elm.c forwards to them. */
#include "gfx/button.h"
#include "gfx/render.h"
#include "core/log.h"
#include "vm/elements.h"
#include "vm/proc.h"

/* ---- stage.6 objbtngroup (FUN_00667e80, ENGINE_SPEC §1.4.4) --------------------------------------------------- */
static int32_t btn_group_value(const BtnGroup *g, int sub, bool *ok)
{
    *ok = true;
    switch (sub) {
    case ELM_GROUP_GET_DECIDED_NO:
    case ELM_GROUP_ON_DECIDED_NO: return g->decided;
    case ELM_GROUP_GET_HIT_NO:
    case ELM_GROUP_ON_HIT_NO: return g->hit_no;
    case ELM_GROUP_GET_PUSHED_NO:
    case ELM_GROUP_ON_PUSHED_NO: return g->pushed_no;
    case ELM_GROUP_ORDER:
    case ELM_GROUP_LAYER:
    case ELM_GROUP_CANCEL_PRIORITY: return 0;
    case ELM_GROUP_GET_RESULT: return g->result;
    case ELM_GROUP_GET_RESULT_BUTTON_NO: return g->result_no;
    default: *ok = false; return 0;
    }
}

ElmResult btn_elm_group(int stage, const int *rest, int nrest, ElmCall *c)
{
    if (nrest > 0 && rest[0] == 6) { rest++; nrest--; }   /* accept the chain with or without the ".6" */
    if (nrest >= 1 && (rest[0] == 1 || rest[0] == 2)) { elm_ret_none(c); return ELM_OK; }   /* alloc / free: fixed 16 */
    if (nrest < 3 || rest[0] != -1) return ELM_UNKNOWN;
    BtnGroup *g = btn_group_get(stage, rest[1]);
    if (!g) {
        log_msg(LOG_WARN, "gfx: objbtngroup[%d] (stage %d) out of range", rest[1], stage);
        if (c->ret_form) elm_push_default(c);
        return ELM_OK;
    }
    btn_init();
    int sub = rest[2], cancel_se = c->npos > 0 ? elm_arg_int(c, 0, -1) : -1;
    bool ok;
    switch (sub) {
    case ELM_GROUP_SEL: btn_group_sel(stage, rest[1], false, -1); return ELM_OK; /* result via proc 0x10 */
    case ELM_GROUP_SEL_CANCEL: btn_group_sel(stage, rest[1], true, cancel_se); return ELM_OK;
    case ELM_GROUP_START: btn_group_start(g); break;
    case ELM_GROUP_START_CANCEL:
        btn_group_start(g);
        g->cancel_ok = true;
        g->cancel_se = cancel_se;
        break;
    case ELM_GROUP_INIT: btn_group_init(g); break;
    case ELM_GROUP_END: btn_group_end(g); break;
    default: {
        int32_t v = btn_group_value(g, sub, &ok);
        if (!ok) return ELM_UNKNOWN;
        elm_ret_int(c, v);
        return ELM_OK;
    }
    }
    elm_ret_none(c);
    return ELM_OK;
}

/* ---- obj.42 set_button and the other button sub-commands of an object ----------------------------------------- */
static void set_button(Obj *o, const ElmCall *c)   /* set_button(no[, group[, action, se]]) (FUN_005fb3b0) */
{
    btn_set(o, elm_arg_int(c, 0, 0), c->npos >= 2 ? elm_arg_int(c, 1, 0) : 0, c->npos >= 4 ? elm_arg_int(c, 2, 0) : 0,
            c->npos >= 4 ? elm_arg_int(c, 3, 0) : 0);
}

static int32_t button_query(const Obj *o, int sub)   /* 188-191: -1 when the object is not a button */
{
    if (!o->is_button) return -1;
    return sub == ELM_OBJECT_GET_BUTTON_NO         ? o->btn_no
         : sub == ELM_OBJECT_GET_BUTTON_GROUP_NO  ? o->btn_group
         : sub == ELM_OBJECT_GET_BUTTON_ACTION_NO ? o->btn_action
                                                  : o->btn_se;
}

ElmResult btn_elm_obj(Obj *o, const int *rest, int nrest, ElmCall *c)
{
    int sub = nrest > 0 ? rest[0] : 42;
    if (!o) { if (c->ret_form) elm_push_default(c); return ELM_OK; }
    switch (sub) {
    case ELM_OBJECT_SET_BUTTON: set_button(o, c); break;
    case ELM_OBJECT_CLEAR_BUTTON:
        o->is_button = false;
        o->btn_state = BTN_NORMAL;
        break;
    case ELM_OBJECT_SET_BUTTON_STATE_NORMAL: o->btn_state = BTN_NORMAL; break;
    case ELM_OBJECT_SET_BUTTON_STATE_SELECT: o->btn_state = BTN_SELECT; break;
    case ELM_OBJECT_SET_BUTTON_STATE_DISABLE: o->btn_state = BTN_DISABLE; break;
    case ELM_OBJECT_GET_BUTTON_STATE:
    case ELM_OBJECT_GET_BUTTON_HIT_STATE:
    case ELM_OBJECT_GET_BUTTON_REAL_STATE: elm_ret_int(c, o->btn_state); return ELM_OK;
    case ELM_OBJECT_SET_BUTTON_GROUP: o->btn_group = elm_arg_int(c, 0, 0); break;
    case ELM_OBJECT_SET_BUTTON_ALPHA_TEST: break; /* alpha test: rect test only */
    case ELM_OBJECT_GET_BUTTON_ALPHA_TEST: elm_ret_int(c, 0); return ELM_OK;
    case ELM_OBJECT_GET_BUTTON_NO:
    case ELM_OBJECT_GET_BUTTON_GROUP_NO:
    case ELM_OBJECT_GET_BUTTON_ACTION_NO:
    case ELM_OBJECT_GET_BUTTON_SE_NO: elm_ret_int(c, button_query(o, sub)); return ELM_OK;
    default: return ELM_UNKNOWN;
    }
    elm_ret_none(c);
    return ELM_OK;
}
