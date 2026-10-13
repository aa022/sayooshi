/* ui/ui_editbox.c — editbox (root 97, SYS §3.4; #EDITBOX.CNT = 4), drawn like the Win32 EDIT control of the
 * original: white field, black text, caret. Text comes from SDL text input through input.c.
 *
 * Memory: UI_EDITBOX_CNT fixed boxes of EB_MAX UTF-16 units each; input beyond the capacity is ignored. */
#include "ui/ui_internal.h"
#include "core/str.h"
#include "gfx/canvas.h"
#include "plat/input.h"
#include "txt/font.h"
#include "vm/clock.h"
#include <SDL3/SDL.h>
#include <string.h>

enum { EB_MAX = 128, EB_SIZE_DEFAULT = 24, EB_CARET_BLINK_MS = 500 };

typedef struct { bool used; Rect r; int size; uint16_t t[EB_MAX]; int n, caret; bool decided, canceled; } EditBox;

static EditBox g_eb[UI_EDITBOX_CNT];
static int g_focus = -1;
static bool g_text_input_on;

/* SDL text input on the focused window (the first window if none has the keyboard). */
static void text_input(bool on)
{
    if (on == g_text_input_on) return;
    g_text_input_on = on;
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return;
    SDL_Window *w = SDL_GetKeyboardFocus();
    if (!w) {
        int n = 0;
        SDL_Window **ws = SDL_GetWindows(&n);
        if (ws && n > 0) w = ws[0];
        SDL_free((void *)ws);                           /* SDL's memory */
    }
    if (!w) return;
    if (on) (void)SDL_StartTextInput(w);                /* best effort: typing just does nothing on failure */
    else (void)SDL_StopTextInput(w);
}

static EditBox *eb(int id) { return id >= 0 && id < UI_EDITBOX_CNT && g_eb[id].used ? &g_eb[id] : NULL; }

void ui_editbox_create(int id, Rect r, int moji_size)
{
    if (id < 0 || id >= UI_EDITBOX_CNT) return;
    EditBox *e = &g_eb[id];
    memset(e, 0, sizeof *e);
    e->used = true;
    e->r = rect_at(r.x0, r.y0, (int64_t)r.x1 - r.x0, (int64_t)r.y1 - r.y0);   /* bounded: draw math cannot overflow */
    e->size = moji_size > 0 && moji_size <= 255 ? moji_size : EB_SIZE_DEFAULT;
}

void ui_editbox_destroy(int id)
{
    if (id < 0 || id >= UI_EDITBOX_CNT) return;
    g_eb[id].used = false;
    if (g_focus == id) { g_focus = -1; text_input(false); }
}

void ui_editbox_set_text(int id, const WStr *s)
{
    EditBox *e = eb(id);
    if (!e) return;
    e->n = s && s->p ? (s->len < EB_MAX ? s->len : EB_MAX) : 0;
    if (e->n > 0) memcpy(e->t, s->p, (size_t)e->n * sizeof(uint16_t));
    e->caret = e->n;
}

bool ui_editbox_get_text(int id, WStr *out)
{
    const EditBox *e = eb(id);
    if (!e) { if (out) ws_set(out, NULL, 0); return false; }
    if (out) ws_set(out, e->t, e->n);
    return true;
}

bool ui_editbox_decided(int id) { const EditBox *e = eb(id); return e && e->decided; }
bool ui_editbox_canceled(int id) { const EditBox *e = eb(id); return e && e->canceled; }
void ui_editbox_clear_input(int id) { EditBox *e = eb(id); if (e) e->decided = e->canceled = false; }
bool ui_editbox_has_focus(void) { return eb(g_focus) != NULL; }

void ui_editbox_set_focus(int id)
{
    if (!eb(id)) return;
    g_focus = id;
    text_input(true);
}

static void insert_unit(EditBox *e, uint16_t c)
{
    if (c < 0x20 || e->n >= EB_MAX) return;
    memmove(&e->t[e->caret + 1], &e->t[e->caret], (size_t)(e->n - e->caret) * sizeof(uint16_t));
    e->t[e->caret++] = c;
    e->n++;
}

void ui_editbox_on_text(const char *utf8)
{
    EditBox *e = eb(g_focus);
    if (!e) return;
    uint16_t mem[EB_MAX + 1];
    WBuf b;
    wb_init(&b, mem, COUNT_OF(mem));
    (void)wb_put_utf8(&b, str_c(utf8));                 /* more than fits in a box is dropped anyway */
    for (size_t i = 0; i < b.n; i++) insert_unit(e, mem[i]);
}

static void delete_at(EditBox *e, int pos)
{
    if (pos < 0 || pos >= e->n) return;
    memmove(&e->t[pos], &e->t[pos + 1], (size_t)(e->n - pos - 1) * sizeof(uint16_t));
    e->n--;
}

static void focus_next(void)
{
    for (int k = 1; k <= UI_EDITBOX_CNT; k++)
        if (eb((g_focus + k) % UI_EDITBOX_CNT)) { g_focus = (g_focus + k) % UI_EDITBOX_CNT; return; }
}

void ui_editbox_on_key(int sdl_scancode)
{
    EditBox *e = eb(g_focus);
    if (!e) return;
    switch (sdl_scancode) {
    case SDL_SCANCODE_BACKSPACE: if (e->caret > 0) delete_at(e, --e->caret); break;
    case SDL_SCANCODE_DELETE:    delete_at(e, e->caret); break;
    case SDL_SCANCODE_LEFT:      if (e->caret > 0) e->caret--; break;
    case SDL_SCANCODE_RIGHT:     if (e->caret < e->n) e->caret++; break;
    case SDL_SCANCODE_HOME:      e->caret = 0; break;
    case SDL_SCANCODE_END:       e->caret = e->n; break;
    case SDL_SCANCODE_RETURN: case SDL_SCANCODE_KP_ENTER: e->decided = true; break;
    case SDL_SCANCODE_ESCAPE:    e->canceled = true; break;
    case SDL_SCANCODE_TAB:       focus_next(); break;
    default: break;
    }
}

/* A left press inside a box focuses it and puts the caret at the clicked character. */
void ui_editbox_mouse(void)
{
    if (!input_query_mouse(0, INPUT_CODE_ON_DOWN)) return;
    for (int i = 0; i < UI_EDITBOX_CNT; i++) {
        EditBox *e = eb(i);
        if (!e || !ui_in(e->r)) continue;
        ui_editbox_set_focus(i);
        int x = input_mouse_x() - e->r.x0 - 2, w = 0;
        e->caret = 0;
        while (e->caret < e->n && w + ui_u16_w(&e->t[e->caret], 1, e->size) / 2 <= x)
            w += ui_u16_w(&e->t[e->caret++], 1, e->size);
        return;
    }
}

static void draw_box(Canvas *fb, const EditBox *e, bool focused)
{
    ui_fill(fb, e->r, UI_COL_WHITE, 255);
    ui_frame_rect(fb, (Rect){e->r.x0 - 1, e->r.y0 - 1, e->r.x1 + 1, e->r.y1 + 1},
                  focused ? UI_COL_EDIT_FOCUS : UI_COL_TEXT_OFF);
    int size = e->size, h = e->r.y1 - e->r.y0;
    if (size > h) size = h;
    int y = e->r.y0 + (h - size) / 2, x = e->r.x0 + 2;
    for (int k = 0; k < e->n; k++) {
        font_draw_char(fb, e->t[k], size, x, y, UI_COL_BLACK, UI_COL_BLACK, UI_COL_BLACK, 0, false, 255);
        x += ui_u16_w(&e->t[k], 1, size);
    }
    if (focused && (clock_real_ms() / EB_CARET_BLINK_MS) % 2 == 0) {
        int cx = e->r.x0 + 2 + ui_u16_w(e->t, e->caret, size);
        ui_fill(fb, (Rect){ cx, y, cx + 1, y + size }, UI_COL_BLACK, 255);
    }
}

void ui_editbox_draw_all(Canvas *fb)
{
    for (int i = 0; i < UI_EDITBOX_CNT; i++) {
        const EditBox *e = eb(i);
        if (e) draw_box(fb, e, i == g_focus);
    }
}
