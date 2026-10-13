/* ui/ui_menu.c — the syscom menu (SYS §5, #SYSCOMMENU.*) and the yes/no confirm box (#WARNINGINFO.*).
 * Labels come from Gameexe (Japanese, or English with the EN patch), falling back to lang.h in the current language.
 *
 * Memory: fixed static labels, read at ui_init. */
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/lang.h"
#include "txt/mwnd.h"
#include "plat/input.h"
#include "vm/sysstate.h"
#include "fmt/gameexe.h"
#include <SDL3/SDL_scancode.h>

enum { MI_READ_SKIP, MI_UNREAD_SKIP, MI_AUTO_SKIP, MI_AUTO_MODE, MI_HIDE_MWND, MI_SAVE, MI_LOAD, MI_RETURN_SEL,
       MI_CONFIG, MI_RETURN_MENU, MI_GAME_END, MI_CANCEL, MI_N };
enum { MENU_W = 300, MENU_ROW = 28, MENU_SIZE = 20, MENU_PAD = 8, MARK = 0x25CF /* ● */ };
enum { CF_W = 400, CF_H = 130, CF_SIZE = 20, CF_BTN_W = 110, CF_BTN_H = 32, CF_BTN_GAP = 150, CF_NO = 1 };

static const struct { const char *key; LangStrId fallback; } MENU_DEF[MI_N] = {
    { "SYSCOMMENU.READ_SKIP", LANG_STR_MENU_READ_SKIP }, { "SYSCOMMENU.UNREAD_SKIP", LANG_STR_MENU_UNREAD_SKIP },
    { "SYSCOMMENU.AUTO_SKIP", LANG_STR_MENU_AUTO_SKIP }, { "SYSCOMMENU.AUTO_MODE", LANG_STR_MENU_AUTO_MODE },
    { "SYSCOMMENU.HIDE_MWND", LANG_STR_MENU_HIDE_MWND }, { "SYSCOMMENU.SAVE", LANG_STR_SAVE },
    { "SYSCOMMENU.LOAD", LANG_STR_LOAD },                { "SYSCOMMENU.RETURN_SEL", LANG_STR_MENU_RETURN_SEL },
    { "SYSCOMMENU.CONFIG", LANG_STR_SETTINGS },          { "SYSCOMMENU.RETURN_MENU", LANG_STR_MENU_RETURN_MENU },
    { "SYSCOMMENU.GAME_END", LANG_STR_MENU_GAME_END },   { "SYSCOMMENU.CANCEL", LANG_STR_MENU_CANCEL },
};

static struct { UiText label; bool exist, enable; } g_item[MI_N];
static int g_vis[MI_N], g_nvis;                     /* visible items in menu order */
static UiText g_warn_menu, g_warn_sel, g_warn_end, g_yes, g_no;
static bool g_unread_skip, g_auto_skip;
static bool g_hidden;                               /* "hide window" until the next click */

void ui_menu_init(void)
{
    g_nvis = 0;
    for (int i = 0; i < MI_N; i++) {
        ui_text_gx(&g_item[i].label, MENU_DEF[i].key, 2, lang_str(MENU_DEF[i].fallback));
        g_item[i].exist = gx_int(MENU_DEF[i].key, 0, 1) != 0;
        g_item[i].enable = gx_int(MENU_DEF[i].key, 1, 1) != 0;
        if (g_item[i].exist) g_vis[g_nvis++] = i;
    }
    ui_text_gx(&g_warn_menu, "WARNINGINFO.RETURNMENU_WARNING_STR", 0, lang_str(LANG_STR_WARN_RETURN_MENU));
    ui_text_gx(&g_warn_sel, "WARNINGINFO.RETURNSEL_WARNING_STR", 0, lang_str(LANG_STR_WARN_RETURN_SEL));
    ui_text_gx(&g_warn_end, "WARNINGINFO.GAMEEND_WARNING_STR", 0, lang_str(LANG_STR_WARN_GAME_END));
    ui_text_set(&g_yes, lang_str(LANG_STR_YES));
    ui_text_set(&g_no, lang_str(LANG_STR_NO));
}

bool ui_menu_hidden(void) { return g_hidden; }
void ui_menu_unhide(void) { g_hidden = false; mwnd_set_user_hidden(false); }
int  ui_menu_count(void) { return g_nvis; }

static int menu_x0(void) { return (SCREEN_W - MENU_W) / 2; }
static int menu_y0(void) { return (SCREEN_H - g_nvis * MENU_ROW - 2 * MENU_PAD) / 2; }

Rect ui_menu_row(int k)
{
    int x0 = menu_x0(), y = menu_y0() + MENU_PAD + k * MENU_ROW;
    return (Rect){ x0, y, x0 + MENU_W, y + MENU_ROW };
}

static bool menu_checked(int id)
{
    switch (id) {
    case MI_READ_SKIP:   return sys_state()->skip_request && !g_unread_skip;
    case MI_UNREAD_SKIP: return g_unread_skip;
    case MI_AUTO_SKIP:   return g_auto_skip;
    case MI_AUTO_MODE:   return sys_state()->auto_mode;
    default:             return false;
    }
}

static bool menu_enabled(int id)
{
    if (!g_item[id].enable) return false;
    return !(id == MI_SAVE && sys_state()->syscom_locked);
}


static void menu_activate(int id)
{
    switch (id) {
    case MI_READ_SKIP:   g_unread_skip = false; sys_set_skip(!sys_state()->skip_request); ui_close_all(); break;
    case MI_UNREAD_SKIP: g_unread_skip = !g_unread_skip; sys_set_skip(g_unread_skip); ui_close_all(); break;
    case MI_AUTO_SKIP:   g_auto_skip = !g_auto_skip; ui_close_all(); break;
    case MI_AUTO_MODE:   sys_state()->auto_mode = !sys_state()->auto_mode; ui_close_all(); break;
    case MI_HIDE_MWND:   g_hidden = true; mwnd_set_user_hidden(true); ui_close_all(); break;
    case MI_SAVE:        ui_open_save(); break;
    case MI_LOAD:        ui_open_load(); break;
    case MI_CONFIG:      ui_open_config(); break;
    case MI_RETURN_SEL:  ui_confirm(&g_warn_sel, UIA_RETURN_SEL, 0); break;
    case MI_RETURN_MENU: ui_confirm(&g_warn_menu, UIA_RETURN_MENU, 0); break;
    case MI_GAME_END:    ui_confirm(&g_warn_end, UIA_GAME_END, 0); break;
    default:             ui_pop(); break;
    }
}

/* s->drag / s->scroll hold the last mouse position: the keyboard keeps the focus until the mouse moves. */
void ui_menu_frame(UiScreen *s)
{
    int mhover = -1;
    for (int k = 0; k < g_nvis; k++)
        if (ui_in(ui_menu_row(k))) mhover = k;
    if (mhover >= 0 && (input_mouse_x() != s->drag || input_mouse_y() != s->scroll)) s->hover = mhover;
    s->drag = input_mouse_x(); s->scroll = input_mouse_y();
    if (input_key_pressed(SDL_SCANCODE_DOWN)) s->hover = ui_cycle(s->hover, g_nvis, 1);
    if (input_key_pressed(SDL_SCANCODE_UP)) s->hover = ui_cycle(s->hover, g_nvis, -1);
    if (ui_rclick()) { ui_pop(); return; }
    bool go = (ui_lclick() && mhover >= 0) || ui_enter_pressed() || input_key_pressed(SDL_SCANCODE_SPACE);
    if (go && s->hover >= 0 && s->hover < g_nvis && menu_enabled(g_vis[s->hover])) menu_activate(g_vis[s->hover]);
}

void ui_menu_draw(Canvas *fb, const UiScreen *s)
{
    int x0 = menu_x0(), y0 = menu_y0();
    Rect box = { x0, y0, x0 + MENU_W, y0 + g_nvis * MENU_ROW + 2 * MENU_PAD };
    static const uint16_t mark = MARK;
    ui_fill(fb, box, UI_COL_MENU, 220);
    ui_frame_rect(fb, box, UI_COL_FRAME);
    for (int k = 0; k < g_nvis; k++) {
        int id = g_vis[k];
        Rect r = ui_menu_row(k);
        if (k == s->hover) ui_fill(fb, (Rect){ r.x0 + 4, r.y0, r.x1 - 4, r.y1 }, UI_COL_HILITE, 200);
        uint32_t col = !menu_enabled(id) ? UI_COL_TEXT_OFF : k == s->hover ? UI_COL_TEXT_HOVER : UI_COL_TEXT;
        if (menu_checked(id)) ui_draw_u16(fb, x0 + 12, r.y0 + 4, MENU_SIZE, col, &mark, 1);
        ui_draw_text(fb, x0 + 40, r.y0 + 4, MENU_SIZE, col, &g_item[id].label);
    }
}

void ui_open_syscom(void)
{
    if (sys_state()->syscom_locked || ui_active()) return;
    if (!ui_inited()) ui_init();
    UiScreen *s = ui_push(UI_SYSCOM);
    if (s) s->hover = -1;
    if (log_trace_on("INPUT")) log_trace("INPUT", "syscom menu open");
}

/* ---- confirm box ---------------------------------------------------------------------------------------------- */
void ui_confirm(const UiText *msg, int action, int arg)
{
    UiScreen *s = ui_push(UI_CONFIRM);
    if (!s) return;
    s->msg = *msg;
    s->action = action;
    s->arg = arg;
    s->hover = CF_NO;                               /* default: いいえ */
}

/* Wide enough for the question (e.g. the language switch), at most the screen minus a margin. */
static int confirm_w(const UiScreen *s)
{
    int w = ui_text_w(&s->msg, CF_SIZE) + 48;
    return w < CF_W ? CF_W : w > SCREEN_W - 20 ? SCREEN_W - 20 : w;
}

Rect ui_confirm_button(int k)
{
    int bx = (SCREEN_W - CF_W) / 2, by = (SCREEN_H - CF_H) / 2;
    int x0 = bx + 70 + k * CF_BTN_GAP, y0 = by + CF_H - 48;
    return (Rect){ x0, y0, x0 + CF_BTN_W, y0 + CF_BTN_H };
}

void ui_confirm_frame(UiScreen *s)
{
    int hit = -1;
    for (int k = 0; k < 2; k++)
        if (ui_in(ui_confirm_button(k))) hit = s->hover = k;
    if (input_key_pressed(SDL_SCANCODE_LEFT) || input_key_pressed(SDL_SCANCODE_RIGHT)) s->hover ^= 1;
    if (ui_rclick()) { ui_pop(); return; }
    bool go = (ui_lclick() && hit >= 0) || ui_enter_pressed();
    if (!go) return;
    int action = s->action, arg = s->arg, choice = s->hover;
    ui_pop();
    if (choice == 0) ui_do_action(action, arg);
}

void ui_confirm_draw(Canvas *fb, const UiScreen *s)
{
    int cw = confirm_w(s), bx = (SCREEN_W - cw) / 2, by = (SCREEN_H - CF_H) / 2;
    Rect box = { bx, by, bx + cw, by + CF_H };
    ui_fill(fb, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, UI_COL_BLACK, 96);
    ui_fill(fb, box, UI_COL_DIALOG, 235);
    ui_frame_rect(fb, box, UI_COL_FRAME);
    ui_draw_text(fb, bx + (cw - ui_text_w(&s->msg, CF_SIZE)) / 2, by + 24, CF_SIZE, UI_COL_TEXT, &s->msg);
    for (int k = 0; k < 2; k++) {
        Rect r = ui_confirm_button(k);
        const UiText *l = k == 0 ? &g_yes : &g_no;
        bool on = k == s->hover;
        ui_fill(fb, r, on ? UI_COL_HILITE : UI_COL_BUTTON, 230);
        ui_frame_rect(fb, r, UI_COL_BUTTON_FRAME);
        ui_draw_text(fb, r.x0 + (CF_BTN_W - ui_text_w(l, CF_SIZE)) / 2, r.y0 + 6, CF_SIZE,
                     on ? UI_COL_TEXT_HOVER : UI_COL_TEXT, l);
    }
}
