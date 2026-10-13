/* txt/sel.c — in-window selection (global 100 selmsg; TXT §4, exe FUN_00599530 / FUN_005ea2c0 / FUN_005e9f40).
 * Layout [U, ENGINE_SPEC §1.6]: one item per line starting at the selection origin, message font and pitch;
 * normal colour = the message colour (COLOR_TABLE[0]), hover = COLOR_TABLE[5].
 * English (SPEC_ENPATCH §1.3.4, hooks H6/H7): each choice is word-wrapped (msg_layout's English rules, continuation
 * lines aligned under the item start), items stay one per row as a vertical list, and once any item wraps every
 * hit rect gets the width of the widest item.
 *
 * Memory: items and their glyphs live in the window's fixed stores; a choice is laid out straight into the free
 * tail of Mwnd.sel_g. */
#include "txt/mwnd.h"
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/lang.h"
#include "vm/proc.h"
#include "plat/input.h"
#include "sav/sav.h"
#include "vm/vm.h"
#include <SDL3/SDL_scancode.h>

enum { SEL_LIMIT_Y = 1 << 20 };                 /* choices never overflow into a new page */

void sel_reset(Mwnd *w)
{
    w->nsel = 0;
    w->nsel_g = 0;
    w->sel_active = false;
    w->sel_cancel = false;
    w->sel_hover = -1;
    w->sel_kb = -1;
    w->sel_mx = w->sel_my = -1;
}

/* H7: once an item wraps, every hit rect gets the width of the widest item. */
static void equalize_widths(Mwnd *w, int line_h)
{
    int maxx = 0;
    bool wrapped = false;
    for (int k = 0; k < w->nsel; k++) {
        if (w->sel[k].r.y1 - w->sel[k].r.y0 > line_h) wrapped = true;
        if (w->sel[k].r.x1 > maxx) maxx = w->sel[k].r.x1;
    }
    if (wrapped)
        for (int k = 0; k < w->nsel; k++) w->sel[k].r.x1 = maxx;
}

/* A message object with the window's metrics that lays out into the free tail of the choice glyph store, one
 * line below the previous item (text space). */
static Msg item_layouter(Mwnd *w, int y0, bool latin)
{
    Msg lm = w->msg;
    msg_bind(&lm, w->sel_g + w->nsel_g, SEL_GLYPH_MAX - w->nsel_g);
    lm.x = w->sel_x; lm.y = y0;
    lm.indent_x = latin ? w->sel_x : 0;                  /* English: wrapped lines continue under the item */
    lm.depth = 0; lm.open_ch = 0; lm.at_line_start = true;
    lm.limit_y = SEL_LIMIT_Y;
    return lm;
}

void sel_add_item(Mwnd *w, const WStr *text, int cond, int type)
{
    static bool logged;
    if (w->nsel >= SEL_ITEM_MAX) { txt_cap_overflow(&logged, "choice list", SEL_ITEM_MAX); return; }
    SelItem *it = &w->sel[w->nsel];
    *it = (SelItem){ .cond = cond, .type = type };
    /* rects are in screen space; oy converts back to text space */
    int ox = w->cfg.win_x + w->cfg.msg_x, oy = w->cfg.win_y + w->cfg.msg_y;
    int y0 = w->nsel > 0 ? w->sel[w->nsel - 1].r.y1 - oy : w->sel_y;
    bool latin = lang_latin_layout();
    Msg lm = item_layouter(w, y0, latin);
    (void)msg_layout(&lm, text->p, text->len);           /* never short: limit_y is out of reach */
    it->g0 = w->nsel_g;
    int x1 = lm.x, y1 = lm.y + lm.size + lm.space_y;
    for (int k = 0; k < lm.ng; k++) {
        int gx1 = lm.g[k].x + msg_char_width(lm.g[k].code, lm.g[k].size);
        if (gx1 > x1) x1 = gx1;
    }
    w->nsel_g += lm.ng;
    it->g1 = w->nsel_g;
    it->r = (Rect){ ox + w->sel_x, oy + y0, ox + (x1 > w->sel_x ? x1 : w->sel_x + 1), oy + y1 };
    w->nsel++;
    if (latin) equalize_widths(w, lm.size + lm.space_y);
}

int mwnd_sel_count(void)
{
    const Mwnd *w = mwnd_cur();
    return w->sel_active ? w->nsel : 0;
}

bool mwnd_sel_item_rect(int k, Rect *out)
{
    const Mwnd *w = mwnd_cur();
    if (!w->sel_active || k < 0 || k >= w->nsel) return false;
    if (out) *out = w->sel[k].r;
    return true;
}

static int item_at(const Mwnd *w, int mx, int my)
{
    for (int k = 0; k < w->nsel; k++) {
        const Rect *r = &w->sel[k].r;
        if (mx >= r->x0 && mx < r->x1 && my >= r->y0 && my < r->y1) return k;
    }
    return -1;
}

/* Up/Down move the keyboard cursor (starting from the item under the mouse); moving the mouse hands it back. */
static void update_hover(Mwnd *w, int mx, int my, int mouse_hit)
{
    if (w->sel_kb >= 0 && (mx != w->sel_mx || my != w->sel_my) && w->sel_mx >= 0) w->sel_kb = -1;
    w->sel_mx = mx; w->sel_my = my;
    if (w->nsel > 0 && input_key_pressed(SDL_SCANCODE_DOWN))
        w->sel_kb = ((w->sel_kb >= 0 ? w->sel_kb : mouse_hit >= 0 ? mouse_hit : -1) + 1) % w->nsel;
    if (w->nsel > 0 && input_key_pressed(SDL_SCANCODE_UP))
        w->sel_kb = ((w->sel_kb >= 0 ? w->sel_kb : mouse_hit >= 0 ? mouse_hit : 0) + w->nsel - 1) % w->nsel;
    w->sel_hover = w->sel_kb >= 0 ? w->sel_kb : mouse_hit;
}

/* FUN_005e9f40: hover by mouse; a decide on an item -> its index; cancel (if allowed) -> -1.
 * Then push the result, pop proc 0x0e and copy the snapshot to the selection history (sel_point).
 * Port addition (ENGINE_SPEC §1.6): Up/Down (the gamepad D-pad, platform.c) move a cursor over the items and
 * Enter/Space decide it; moving the mouse hands the cursor back to the mouse. A mouse click still needs the pointer
 * on an item. */
void sel_frame(bool screen_open)
{
    Mwnd *w = mwnd_cur();
    if (!w->sel_active || !w->open || w->open_anim >= 0) return;
    Proc *p = proc_top();
    if (!p || p->type != PROC_SEL || screen_open) return;
    int mx = input_mouse_x(), my = input_mouse_y(), mouse_hit = item_at(w, mx, my);
    update_hover(w, mx, my, mouse_hit);
    bool mouse_click = input_query_mouse(0, INPUT_CODE_CLICK) != 0;
    int pick = mouse_click ? mouse_hit : w->sel_hover;
    int result;
    if (w->sel_cancel && input_cancel_clicked()) result = -1;
    else if (pick >= 0 && input_decide_clicked()) { input_consume_decide(); result = pick; }
    else return;
    bool push = p->push_result;
    w->sel_active = false;
    proc_pop();
    if (push) vm_push_int(result);
    log_msg(LOG_DEBUG, "sel: decided %d", result);
    sav_selpoint();
}
