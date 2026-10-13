/* ui/langsel.c — boot-time language selector (task: "language selector at the beginning"; docs/SPEC_ENPATCH.md §6.1).
 *
 * Drawn with the engine's own canvas and font module before any game data is loaded: black, like the game's
 * CRAFTWORK logo screen, with the two languages each written in its own script and face (日本語 in the Japanese
 * face, English in the EN patch face), the highlighted one white with thin rules on both sides in the purple of
 * the title screen's marker. Mouse hover/click, Up/Down/Left/Right/Tab + Enter/Space, 1/2, and the gamepad (D-pad +
 * A through platform.c) select. 250 ms fades in and out.
 *
 * The application drives it one frame at a time (ui_langsel_begin / _frame / _finish) through its own frame loop and
 * frame buffer. Memory: the selector state and stack labels; nothing is allocated. */
#include "ui/ui.h"
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/lang.h"
#include "txt/font.h"
#include "plat/input.h"
#include <SDL3/SDL_scancode.h>
#include <string.h>

enum { LS_SIZE = 28, LS_HINT = 15, LS_Y0 = 214, LS_PITCH = 56, LS_BOX_W = 260, LS_FADE_MS = 250, LS_DT_MAX = 100,
       LS_LANGS = 2 };

typedef struct {
    LangId langs[LS_LANGS];
    int n, sel, chosen;
    int last_mx, last_my;
    int fade_in, fade_out;      /* ms; fade_out < 0 until a choice was made */
    int frames;
} LangSel;

static LangSel g_ls;

/* The selector's own colours: the purple of the title screen's marker, greys on black. */
static const uint32_t LS_RULE = 0xFF6A2C9C, LS_MARK = 0xFF8A4CC0;   /* 0xAARRGGBB */
enum { LS_COL_HINT = 0x8C8C98, LS_COL_IDLE = 0x5C5C66, LS_COL_KEYS = 0x50505A };

static void draw_text(Canvas *fb, int face, int x, int y, int size, uint32_t rgb, const UiText *t)
{
    ui_draw_u16_face(fb, face, x, y, size, rgb, t->u, t->n);
}

static void draw_centered(Canvas *fb, int face, int y, int size, uint32_t rgb, const char *utf8)
{
    UiText t;
    ui_text_set(&t, utf8);
    draw_text(fb, face, (SCREEN_W - ui_text_w(&t, size)) / 2, y, size, rgb, &t);
}

static Rect option_box(int k)
{
    int x0 = (SCREEN_W - LS_BOX_W) / 2, y0 = LS_Y0 + k * LS_PITCH - 10;
    return (Rect){ x0, y0, x0 + LS_BOX_W, y0 + LS_SIZE + 20 };
}

/* Rules on both sides of the highlighted name, like the title screen's lines. */
static void draw_rules(Canvas *fb, int x, int w, int y)
{
    int ly = y + LS_SIZE / 2;
    canvas_fill(fb, (Rect){ x - 96, ly, x - 24, ly + 1 }, LS_RULE, 255);
    canvas_fill(fb, (Rect){ x + w + 24, ly, x + w + 96, ly + 1 }, LS_RULE, 255);
    canvas_fill(fb, (Rect){ x - 30, ly - 2, x - 25, ly + 3 }, LS_MARK, 255);
    canvas_fill(fb, (Rect){ x + w + 25, ly - 2, x + w + 30, ly + 3 }, LS_MARK, 255);
}

static void draw_screen(Canvas *fb, const LangSel *ls, int alpha)
{
    canvas_clear(fb, 0xFF000000u);
    draw_centered(fb, FONT_JA, 138, LS_HINT, LS_COL_HINT, "言語を選択してください");
    draw_centered(fb, FONT_EN, 162, LS_HINT, LS_COL_HINT, "Choose your language");
    for (int k = 0; k < ls->n; k++) {
        UiText name;
        ui_text_set(&name, lang_native_name(ls->langs[k]));
        int face = ls->langs[k] == LANG_EN ? FONT_EN : FONT_JA;
        int w = ui_text_w(&name, LS_SIZE), x = (SCREEN_W - w) / 2, y = LS_Y0 + k * LS_PITCH;
        bool on = k == ls->sel;
        if (on) draw_rules(fb, x, w, y);
        draw_text(fb, face, x, y, LS_SIZE, on ? UI_COL_TEXT : LS_COL_IDLE, &name);
    }
    draw_centered(fb, FONT_EN, 440, 12, LS_COL_KEYS, "\xe2\x86\x91\xe2\x86\x93 / Enter");   /* ↑↓ / Enter */
    if (alpha < 255) canvas_fill(fb, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, 0xFF000000u, 255 - alpha);
}

static bool any_pressed(const int *keys, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (input_key_pressed(keys[i])) return true;
    return false;
}

/* Mouse hover (only when the mouse moved onto an item), keys and click; sets ls->chosen on a decision. */
static void handle_input(LangSel *ls, int max_frames)
{
    static const int prev[] = { SDL_SCANCODE_UP, SDL_SCANCODE_LEFT };
    static const int next[] = { SDL_SCANCODE_DOWN, SDL_SCANCODE_RIGHT, SDL_SCANCODE_TAB };
    static const int decide[] = { SDL_SCANCODE_RETURN, SDL_SCANCODE_KP_ENTER, SDL_SCANCODE_SPACE, SDL_SCANCODE_1,
                                  SDL_SCANCODE_2 };
    int mx = input_mouse_x(), my = input_mouse_y(), hit = -1;
    for (int k = 0; k < ls->n; k++)
        if (ui_in(option_box(k))) hit = k;
    if (hit >= 0 && (mx != ls->last_mx || my != ls->last_my)) ls->sel = hit;
    ls->last_mx = mx; ls->last_my = my;
    if (any_pressed(prev, COUNT_OF(prev))) ls->sel = (ls->sel + ls->n - 1) % ls->n;
    if (any_pressed(next, COUNT_OF(next))) ls->sel = (ls->sel + 1) % ls->n;
    if (input_key_pressed(SDL_SCANCODE_1)) ls->sel = 0;
    if (input_key_pressed(SDL_SCANCODE_2) && ls->n > 1) ls->sel = 1;
    if (any_pressed(decide, COUNT_OF(decide)) || (hit >= 0 && input_query_mouse(0, INPUT_CODE_CLICK)))
        ls->chosen = ls->sel;
    if (max_frames > 0 && ls->frames >= max_frames && ls->chosen < 0) {
        log_msg(LOG_INFO, "langsel: no choice after %d frames; using %s", ls->frames, lang_code_of(ls->langs[ls->sel]));
        ls->chosen = ls->sel;
    }
}

static int fade_alpha(const LangSel *ls)
{
    int alpha = ls->fade_in >= LS_FADE_MS ? 255 : 255 * ls->fade_in / LS_FADE_MS;
    if (ls->fade_out >= 0) {
        int a2 = 255 - 255 * (ls->fade_out > LS_FADE_MS ? LS_FADE_MS : ls->fade_out) / LS_FADE_MS;
        if (a2 < alpha) alpha = a2;
    }
    return alpha;
}

/* One frame (the input edges of the frame are already applied): choice, fades, draw. */
static void step(LangSel *ls, Canvas *fb, int dt, int max_frames)
{
    ls->frames++;
    if (ls->chosen < 0) {
        handle_input(ls, max_frames);
        if (ls->chosen >= 0) ls->fade_out = 0;
        ls->fade_in += dt;
    } else {
        ls->fade_out += dt;
    }
    draw_screen(fb, ls, fade_alpha(ls));
}

/* The languages the data offers; 0 or 1 means there is nothing to choose. */
static void list_languages(LangSel *ls, LangId preselect)
{
    memset(ls, 0, sizeof *ls);
    if (lang_available(LANG_JA)) ls->langs[ls->n++] = LANG_JA;
    if (lang_available(LANG_EN)) ls->langs[ls->n++] = LANG_EN;
    for (int k = 0; k < ls->n; k++)
        if (preselect == ls->langs[k]) ls->sel = k;
    ls->chosen = -1;
    ls->last_mx = ls->last_my = -1;
    ls->fade_out = -1;
}

LangId ui_langsel_begin(LangId preselect)
{
    list_languages(&g_ls, preselect);
    if (g_ls.n == 0) return LANG_JA;
    if (g_ls.n == 1) return g_ls.langs[0];
    log_msg(LOG_INFO, "langsel: selector shown (preselected %s)", lang_code_of(g_ls.langs[g_ls.sel]));
    return LANG_NONE;
}

bool ui_langsel_frame(Canvas *fb, int dt, int max_frames)
{
    step(&g_ls, fb, dt, max_frames);
    return g_ls.fade_out < LS_FADE_MS;        /* ends when a choice has faded out (max_frames forces one) */
}

LangId ui_langsel_finish(bool quit)
{
    if (quit && g_ls.chosen < 0) g_ls.chosen = g_ls.sel;
    input_consume_edges();
    const char *code = lang_code_of(g_ls.langs[g_ls.chosen]);
    log_msg(LOG_INFO, "langsel: chose %s after %d frames", code, g_ls.frames);
    log_trace("INPUT", "langsel chose %s", code);
    return g_ls.langs[g_ls.chosen];
}
