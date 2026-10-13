/* ui/ui.c — system UI driver: screen stack, frame/draw dispatch, actions, input hook, config sync, lifetime
 * (ENGINE_SPEC §2.12, WP6). The original uses Win32 menus/dialogs (SYS §5); the port draws a minimal UI with the
 * font module. The screens are in ui_menu.c, ui_saveload.c, ui_settings.c, ui_backlog.c and ui_editbox.c.
 *
 * Memory: the stack is a fixed static array of UI_STACK screens; ui_shutdown releases the save dialog's data. */
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/lang.h"
#include "txt/mwnd.h"
#include "txt/backlog.h"
#include "plat/input.h"
#include "gfx/button.h"
#include "vm/proc.h"
#include "vm/sysstate.h"
#include "vm/elements.h"
#include "snd/audio.h"
#include "sav/sav.h"
#include <string.h>

enum { UI_STACK = 6, ELM_RETURNMENU = 3, ELM_OWARI = 2 };

static UiScreen g_stack[UI_STACK];
static int g_depth;
static bool g_inited, g_selftest_running;
static bool g_engine_cursor = true;

/* ---- screen stack ----------------------------------------------------------------------------------------------- */
UiScreen *ui_push(int kind)
{
    if (g_depth >= UI_STACK) return NULL;
    if (g_depth == 0) sav_thumb_freeze();   /* the last presented frame is still the game without any UI */
    UiScreen *s = &g_stack[g_depth++];
    memset(s, 0, sizeof *s);
    s->kind = kind;
    s->hover = -1;
    return s;
}

void ui_pop(void)
{
    if (g_depth == 0) return;
    g_depth--;
    if (g_depth == 0) sav_thumb_thaw();
}
UiScreen *ui_top(void) { return g_depth > 0 ? &g_stack[g_depth - 1] : NULL; }
bool ui_active(void) { return g_depth > 0; }
void ui_close_all(void)
{
    g_depth = 0;
    sav_thumb_thaw();
}
void ui_set_engine_cursor(bool on) { g_engine_cursor = on; }
bool ui_engine_cursor(void) { return g_engine_cursor; }
bool ui_inited(void) { return g_inited; }

bool ui_test_set_inited(bool inited)
{
    bool was = g_inited;
    g_inited = inited;
    return was;
}

void ui_set_selftest_running(bool on) { g_selftest_running = on; }

/* Actions run after the UI closes; the script-side ones go through the normal element path. */
static void dispatch_global(int root)
{
    ElmCall c;
    memset(&c, 0, sizeof c);
    c.op = OP_COMMAND;
    const int chain[1] = { root };
    ui_close_all();
    elm_dispatch(chain, 1, &c);
}

void ui_do_action(int action, int arg)
{
    switch (action) {
    case UIA_RETURN_MENU: dispatch_global(ELM_RETURNMENU); break;
    case UIA_GAME_END:    dispatch_global(ELM_OWARI); break;
    case UIA_RETURN_SEL:  ui_close_all(); if (!sav_return_sel()) log_msg(LOG_INFO, "ui: no previous choice"); break;
    case UIA_SAVE:
        if (sav_slot_write(arg)) { sav_global_write(); ui_close_all(); }
        else log_msg(LOG_WARN, "ui: save to slot %d failed", arg);
        break;
    case UIA_LOAD:
        ui_close_all();
        if (!sav_slot_load(arg)) log_msg(LOG_WARN, "ui: load of slot %d failed", arg);
        break;
    case UIA_LANG:                       /* arg 0 = ja, 1 = en: main reboots the data at the next frame */
        ui_close_all();
        lang_request_switch(arg ? LANG_EN : LANG_JA);
        break;
    default: break;
    }
}

/* ---- opening screens -------------------------------------------------------------------------------------------- */
void ui_open_save(void)
{
    if (sys_state()->syscom_locked) return;
    UiScreen *s = ui_push(UI_SAVE);
    if (s) ui_saveload_open(s);
}

void ui_open_load(void)
{
    UiScreen *s = ui_push(UI_LOAD);
    if (s) ui_saveload_open(s);
}

void ui_open_config(void)
{
    UiScreen *s = ui_push(UI_CONFIG);
    if (s) ui_settings_open(s);
}

static void open_backlog(void)
{
    UiScreen *s = ui_push(UI_BACKLOG);
    if (s) ui_backlog_open(s);
}

/* ---- frame / draw ------------------------------------------------------------------------------------------------ */
void ui_frame(void)
{
    UiScreen *s = ui_top();
    if (s) {
        switch (s->kind) {
        case UI_SYSCOM:  ui_menu_frame(s); break;
        case UI_CONFIRM: ui_confirm_frame(s); break;
        case UI_SAVE: case UI_LOAD: ui_saveload_frame(s); break;
        case UI_CONFIG:  ui_settings_frame(s); break;
        case UI_BACKLOG: ui_backlog_frame(s); break;
        default:         ui_pop(); break;
        }
    }
    input_consume_edges();           /* nothing the UI saw leaks into the game this frame */
}

void ui_draw(Canvas *fb)
{
    ui_editbox_draw_all(fb);
    for (int i = 0; i < g_depth; i++) {
        const UiScreen *s = &g_stack[i];
        switch (s->kind) {
        case UI_SYSCOM:  ui_menu_draw(fb, s); break;
        case UI_CONFIRM: ui_confirm_draw(fb, s); break;
        case UI_SAVE: case UI_LOAD: ui_saveload_draw(fb, s); break;
        case UI_CONFIG:  ui_settings_draw(fb, s); break;
        case UI_BACKLOG: ui_backlog_draw(fb, s); break;
        default: break;
        }
    }
}

/* ---- per-frame hook from input_frame (ENGINE_SPEC §2.12, §1.6 port decisions) ------------------------------------ */
static bool proc_is_user_wait(void)
{
    const Proc *t = proc_top();
    if (!t) return false;
    switch (t->type) {
    case PROC_KEY_WAIT: case PROC_SEL: case PROC_MSG_WAIT: case PROC_TIMEWAIT: case PROC_KOE_WAIT: case PROC_WIPE_WAIT:
        return true;
    case PROC_BTNSEL: {
        const BtnGroup *g = btn_group_get(t->iarg[0], t->iarg[1]);
        return g && !g->cancel_ok;
    }
    default:
        return false;
    }
}

/* Any click brings the hidden window back and is used up. */
static void unhide_on_click(void)
{
    if (!input_decide_clicked() && !input_cancel_clicked() && input_wheel() == 0) return;
    ui_menu_unhide();
    input_consume_decide();
    input_consume_cancel();
}

void ui_input_hook(void)
{
    ui_editbox_mouse();
    if (!g_inited || g_selftest_running || g_depth > 0) return;
    if (ui_menu_hidden()) { unhide_on_click(); return; }
    if (sys_state()->syscom_locked || !proc_is_user_wait()) return;
    if (input_take_menu_toggle()) {
        ui_open_syscom();
    } else if (input_wheel() > 0) {
        const Proc *t = proc_top();
        if (t && (t->type == PROC_KEY_WAIT || t->type == PROC_SEL) && backlog_count() > 0) open_backlog();
    }
    if (g_depth > 0) input_consume_edges();   /* the opening click/wheel must not act inside the new screen */
}

/* ---- config ------------------------------------------------------------------------------------------------------ */
void ui_config_apply(void)
{
    const SavConfig *sc = sav_config();
    sav_config_apply();                 /* audio, sys_state() speeds, platform window mode (fullscreen) */
    mwnd_set_font_style(sc->font_shadow, sc->font_futoku);
    mwnd_set_options(sc->no_mwnd_anime, sc->koe_dont_stop, sc->wheel_next, sc->msg_nowait);
    gfx_set_wipe_options(sc->skip_wipe_anime, sc->no_wipe_anime);
}

/* ---- lifetime ---------------------------------------------------------------------------------------------------- */
void ui_input_attach(void)
{
    static const InputHooks HOOKS = { ui_editbox_on_text, ui_editbox_on_key, ui_input_hook, ui_editbox_has_focus };
    input_set_hooks(&HOOKS);
}

void ui_init(void)
{
    ui_input_attach();
    ui_elm_register();
    g_inited = true;
    btn_init();
    ui_menu_init();
}

void ui_shutdown(void)
{
    ui_close_all();
    ui_saveload_release();
    for (int i = 0; i < UI_EDITBOX_CNT; i++) ui_editbox_destroy(i);
    input_set_hooks(NULL);
    g_inited = false;
}
