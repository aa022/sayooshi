/* plat/input.c — input edge states (SYS §3.1).
 * One record per button {is_down, on_down, on_up, down_up}. Raw events between two frames are folded into
 * per-button "pressed"/"released" flags and applied once per frame (FUN_006a6d20): first the press, then the
 * release, so a press+release inside one frame yields on_down = on_up = 1, down_up = 2, is_down = 0.
 *
 * Memory: fixed static tables. The editbox event queue holds EVQ_MAX events per frame (more are dropped) and
 * text events of up to EVQ_TEXT - 1 bytes (longer IME commits are cut at a code point). */
#include "plat/input.h"
#include "core/str.h"
#include <SDL3/SDL_scancode.h>
#include <string.h>

enum { SLOT_ML = 0, SLOT_MR = 1, SLOT_KEY0 = 2, NKEYS = SDL_SCANCODE_COUNT, NSLOT = SLOT_KEY0 + NKEYS };
enum { EVQ_MAX = 64, EVQ_TEXT = 64 };

typedef struct { bool pressed, released, phys; } Pending;
typedef struct { bool is_text; int sc; char text[EVQ_TEXT]; } QEvent;   /* editbox stream, in order */

static InputBtn g_btn[NSLOT];
static Pending  g_pend[NSLOT];
static int g_mx = -1, g_my = -1, g_pmx = -1, g_pmy = -1;
static int g_wheel, g_pwheel;
static QEvent g_q[EVQ_MAX];
static int g_nq;
static InputHooks g_hooks;

void input_set_hooks(const InputHooks *h)
{
    if (h) g_hooks = *h;
    else memset(&g_hooks, 0, sizeof g_hooks);
}

/* DECIDE = mouse-L | Return | KP_Enter | Space; CANCEL = mouse-R | Escape (SYS §3.1). */
static const int DECIDE_KEYS[] = { SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_SPACE };
static const int CANCEL_KEYS[] = { SDL_SCANCODE_ESCAPE };

static bool sc_ok(int sc) { return sc >= 0 && sc < NKEYS; }

static void q_push(bool is_text, int sc, const char *text)
{
    if (g_nq >= EVQ_MAX) return;
    QEvent *e = &g_q[g_nq++];
    e->is_text = is_text;
    e->sc = sc;
    bool whole = str_copy_c(e->text, sizeof e->text, text);   /* NULL -> ""; a cut keeps whole code points */
    (void)whole;
}

void input_raw_mouse(int win_x, int win_y) { g_pmx = win_x; g_pmy = win_y; }

void input_raw_button(int which, bool down)
{
    if (which != 0 && which != 1) return;
    Pending *p = &g_pend[which == 0 ? SLOT_ML : SLOT_MR];
    if (down) p->pressed = true; else p->released = true;
    p->phys = down;
}

void input_raw_key(int sdl_scancode, bool down)
{
    int sc = sdl_scancode;
    if (!sc_ok(sc)) return;
    Pending *p = &g_pend[SLOT_KEY0 + sc];
    if (down) q_push(false, sc, NULL);               /* editbox sees OS key repeats too */
    if (down && (p->phys || g_btn[SLOT_KEY0 + sc].is_down) && !p->released) return;   /* auto-repeat: no new edge */
    if (down) p->pressed = true; else p->released = true;
    p->phys = down;
}

void input_raw_wheel(int dy) { g_pwheel += dy; }
void input_raw_text(const char *utf8) { if (utf8 && *utf8) q_push(true, 0, utf8); }

/* Step 1 (FUN_006a6a50): fired edges go back to 0. */
static void consume_edges(void)
{
    for (int i = 0; i < NSLOT; i++) {
        InputBtn *b = &g_btn[i];
        if (b->on_down == 1) b->on_down = 0;
        if (b->on_up == 1) b->on_up = 0;
        if (b->down_up == 2) b->down_up = 0;
    }
    g_wheel = 0;
}

/* Step 2 (FUN_006a6d20): apply the folded raw events, press before release. */
static void apply_pending(void)
{
    for (int i = 0; i < NSLOT; i++) {
        Pending *p = &g_pend[i];
        if (!p->pressed && !p->released) continue;
        InputBtn *b = &g_btn[i];
        if (p->pressed) {
            b->is_down = 1;
            if (b->on_down == 0) b->on_down = 1;
            if (b->down_up == 0) b->down_up = 1;
        }
        if (p->released) {
            b->is_down = 0;
            if (b->on_up == 0) b->on_up = 1;
            if (b->down_up == 1) b->down_up = 2;
        }
        if (p->pressed && p->released && p->phys) b->is_down = 1;   /* release then press: still held */
        p->pressed = p->released = false;
    }
    g_mx = g_pmx;
    g_my = g_pmy;
    g_wheel += g_pwheel;
    g_pwheel = 0;
}

static void drain_queue(void)
{
    for (int i = 0; i < g_nq; i++) {
        if (g_q[i].is_text) { if (g_hooks.on_text) g_hooks.on_text(g_q[i].text); }
        else if (g_hooks.on_key) g_hooks.on_key(g_q[i].sc);
    }
    g_nq = 0;
}

void input_frame(void)
{
    consume_edges();
    apply_pending();
    drain_queue();
    if (g_hooks.after_frame) g_hooks.after_frame();
}

void input_next(void)          /* 86.3 / mouse.8: steps 1+2 right now */
{
    consume_edges();
    apply_pending();
}

/* Zero the current and queued edges of slots [from, to] and the wheel. */
static void clear_edges(int from, int to)
{
    for (int i = from; i <= to; i++) {
        g_btn[i].on_down = g_btn[i].on_up = g_btn[i].down_up = 0;
        g_pend[i].pressed = g_pend[i].released = false;
    }
    g_wheel = g_pwheel = 0;
}

void input_clear(void) { clear_edges(0, NSLOT - 1); }   /* 86.2: every edge (FUN_006a69e0 + 6a6b00) */
void input_mouse_clear(void) { clear_edges(SLOT_ML, SLOT_MR); }   /* mouse.4 */

/* Uses up this frame's edges. A press still held (down_up 1) stays armed: zeroing it would turn the release into
 * a non-click, so a menu clicked across two frames (any real click) would never see it. */
void input_consume_edges(void)
{
    for (int i = 0; i < NSLOT; i++) {
        InputBtn *b = &g_btn[i];
        b->on_down = b->on_up = 0;
        if (b->down_up == 2) b->down_up = 0;
    }
    g_wheel = 0;
}

void input_reset(void)
{
    memset(g_btn, 0, sizeof g_btn);
    memset(g_pend, 0, sizeof g_pend);
    g_mx = g_my = g_pmx = g_pmy = -1;
    g_wheel = g_pwheel = 0;
    g_nq = 0;
}

static int test_btn(const InputBtn *b, int code)
{
    switch (code) {
    case INPUT_CODE_ON_DOWN: return b->on_down == 1;
    case INPUT_CODE_ON_UP:   return b->on_up == 1;
    case INPUT_CODE_CLICK:   return b->down_up == 2;
    case INPUT_CODE_IS_DOWN: return b->is_down == 1;
    case INPUT_CODE_IS_UP:   return b->is_down == 0;
    default:                 return 0;   /* flick / repeat codes: never (keyboard + mouse PC build) */
    }
}

/* Virtual key = OR over its members; IS_UP = no member down. Keyboard members are muted while an editbox
 * has the focus (the original's Win32 EDIT control eats the keys). */
static bool any_member(int mouse_slot, const int *keys, int nkeys, int code)
{
    bool kb = !(g_hooks.keyboard_captured && g_hooks.keyboard_captured());
    if (test_btn(&g_btn[mouse_slot], code)) return true;
    for (int i = 0; kb && i < nkeys; i++)
        if (test_btn(&g_btn[SLOT_KEY0 + keys[i]], code)) return true;
    return false;
}

static int query_vk(int mouse_slot, const int *keys, int nkeys, int code)
{
    if (code == INPUT_CODE_IS_UP) return !any_member(mouse_slot, keys, nkeys, INPUT_CODE_IS_DOWN);
    return any_member(mouse_slot, keys, nkeys, code);
}

int input_query(int vk, int code)
{
    if (vk == VK_DECIDE) return query_vk(SLOT_ML, DECIDE_KEYS, (int)COUNT_OF(DECIDE_KEYS), code);
    if (vk == VK_CANCEL) return query_vk(SLOT_MR, CANCEL_KEYS, (int)COUNT_OF(CANCEL_KEYS), code);
    if (vk >= 0 && vk < 0x100) return input_query_key(vk, code);
    return 0;
}

int input_query_mouse(int which, int code)
{
    if (which != 0 && which != 1) return 0;
    return test_btn(&g_btn[which == 0 ? SLOT_ML : SLOT_MR], code);
}

int input_query_key(int sdl_scancode, int code)
{
    return sc_ok(sdl_scancode) ? test_btn(&g_btn[SLOT_KEY0 + sdl_scancode], code) : 0;
}
const InputBtn *input_btn_mouse(int which) { return &g_btn[which == 0 ? SLOT_ML : SLOT_MR]; }
bool input_key_pressed(int sdl_scancode)
{
    return sc_ok(sdl_scancode) && g_btn[SLOT_KEY0 + sdl_scancode].on_down == 1;
}

/* A key press that triggers an action right away (menu open/close on key down) is used up whole: its press edge
 * goes, and its click is disarmed, so the release cannot act a second time (e.g. reopen the menu it closed). */
bool input_take_key_press(int sdl_scancode)
{
    if (!input_key_pressed(sdl_scancode)) return false;
    InputBtn *b = &g_btn[SLOT_KEY0 + sdl_scancode];
    b->on_down = 0;
    b->down_up = 0;
    return true;
}

/* System-menu toggle: a right click (on release, like the original) or Escape on key down; either is used up. */
bool input_take_menu_toggle(void)
{
    if (input_take_key_press(SDL_SCANCODE_ESCAPE)) return true;
    if (!input_query_mouse(1, INPUT_CODE_CLICK)) return false;
    g_btn[SLOT_MR].down_up = 0;
    return true;
}

bool input_decide_clicked(void) { return input_query(VK_DECIDE, INPUT_CODE_CLICK) != 0; }
bool input_cancel_clicked(void) { return input_query(VK_CANCEL, INPUT_CODE_CLICK) != 0; }

/* A used-up click: down_up 2 -> 0 on the mouse slot and the keyboard members. */
static void consume_clicks(int mouse_slot, const int *keys, size_t nkeys)
{
    if (g_btn[mouse_slot].down_up == 2) g_btn[mouse_slot].down_up = 0;
    for (size_t i = 0; i < nkeys; i++)
        if (g_btn[SLOT_KEY0 + keys[i]].down_up == 2) g_btn[SLOT_KEY0 + keys[i]].down_up = 0;
}

/* FUN_006a6920: the click edge that satisfied a wait is used up, so one click ends exactly one wait. */
void input_consume_decide(void) { consume_clicks(SLOT_ML, DECIDE_KEYS, COUNT_OF(DECIDE_KEYS)); }
void input_consume_cancel(void) { consume_clicks(SLOT_MR, CANCEL_KEYS, COUNT_OF(CANCEL_KEYS)); }

int input_mouse_x(void) { return g_mx; }
int input_mouse_y(void) { return g_my; }
int input_wheel(void) { return g_wheel; }

bool input_ctrl_held(void)
{
    return g_btn[SLOT_KEY0 + SDL_SCANCODE_LCTRL].is_down || g_btn[SLOT_KEY0 + SDL_SCANCODE_RCTRL].is_down;
}
