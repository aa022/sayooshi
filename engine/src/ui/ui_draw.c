/* ui/ui_draw.c — bounded UI text (UiText), the drawing and hit-test helpers every ui screen uses, and the mouse
 * cursor (#MOUSE_CURSOR.000: _mouse_00, 8 cuts, 160 ms; ENGINE_SPEC §2.7/§2.9 step 4).
 *
 * Memory: none. UiText is fixed storage filled through a core WBuf; Gameexe strings are read into a temporary
 * WStr that is freed before returning. */
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "plat/input.h"
#include "txt/font.h"
#include "fmt/gameexe.h"
#include "vm/clock.h"
#include <SDL3/SDL_scancode.h>

enum { CURSOR_NAME_MAX = 64, CURSOR_CUTS = 8, CURSOR_SPEED_MS = 160 };

/* ---- UiText ------------------------------------------------------------------------------------------------- */
static WBuf text_buf(UiText *t)
{
    WBuf b;
    wb_init(&b, t->u, COUNT_OF(t->u));
    return b;
}

static void text_done(UiText *t, const WBuf *b) { t->n = (int)b->n; }

void ui_text_set(UiText *t, const char *utf8)
{
    WBuf b = text_buf(t);
    (void)wb_put_utf8(&b, str_c(utf8));             /* invalid bytes -> U+FFFD; overlong labels are cut */
    text_done(t, &b);
}

static void ui_text_set_ws(UiText *t, const WStr *s)
{
    WBuf b = text_buf(t);
    if (s && s->p && s->len > 0) (void)wb_put(&b, s->p, (size_t)s->len);   /* cut at UI_TEXT_MAX */
    text_done(t, &b);
}

static bool text_from_gx(UiText *t, const char *key, int field)
{
    WStr w = {0};
    bool ok = gx_str(key, field, &w) && w.p && w.len > 0;
    if (ok) ui_text_set_ws(t, &w);
    ws_free(&w);
    return ok;
}

void ui_text_gx(UiText *t, const char *key, int field, const char *fallback_utf8)
{
    if (text_from_gx(t, key, field)) return;
    if (field != 0 && text_from_gx(t, key, 0)) return;
    ui_text_set(t, fallback_utf8);
}

/* ---- drawing ------------------------------------------------------------------------------------------------ */
static int char_w(uint16_t c, int size)
{
    bool half = c < 0x80 || (c >= 0xFF61 && c <= 0xFF9F);   /* ENGINE_SPEC §1.6: CP932 single byte */
    return half ? size / 2 : size;
}

int ui_u16_w(const uint16_t *s, int n, int size)
{
    int w = 0;
    for (int i = 0; i < n; i++) w += char_w(s[i], size);
    return w;
}

void ui_draw_u16(Canvas *fb, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] != ' ' && s[i] != 0x3000)
            font_draw_char(fb, s[i], size, x, y, rgb, UI_COL_BLACK, UI_COL_BLACK, 2, false, 255);
        x += char_w(s[i], size);
    }
}

void ui_draw_u16_face(Canvas *fb, int face, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n)
{
    for (int i = 0; i < n; i++) {
        if (s[i] != ' ' && s[i] != 0x3000)
            font_draw_char_face(fb, face, s[i], size, x, y, rgb, UI_COL_BLACK, UI_COL_BLACK, 0, 255);
        x += char_w(s[i], size);
    }
}

/* The longest prefix of s that fits in maxw pixels. */
void ui_draw_clipped(Canvas *fb, int x, int y, int size, uint32_t rgb, const uint16_t *s, int n, int maxw)
{
    int k = 0, w = 0;
    while (k < n && w + char_w(s[k], size) <= maxw) w += char_w(s[k++], size);
    ui_draw_u16(fb, x, y, size, rgb, s, k);
}

void ui_fill(Canvas *fb, Rect r, uint32_t rgb, int tr) { canvas_fill(fb, r, 0xFF000000u | rgb, tr); }

void ui_frame_rect(Canvas *fb, Rect r, uint32_t rgb)
{
    ui_fill(fb, (Rect){ r.x0, r.y0, r.x1, r.y0 + 1 }, rgb, 255);
    ui_fill(fb, (Rect){ r.x0, r.y1 - 1, r.x1, r.y1 }, rgb, 255);
    ui_fill(fb, (Rect){ r.x0, r.y0, r.x0 + 1, r.y1 }, rgb, 255);
    ui_fill(fb, (Rect){ r.x1 - 1, r.y0, r.x1, r.y1 }, rgb, 255);
}

void ui_image(Canvas *fb, const char *name, int pat, int x, int y, int tr)
{
    const Image *img = img_get(name, pat);
    if (!img) return;
    canvas_blit(fb, img, (Rect){ 0, 0, img->w, img->h }, x - img->cx, y - img->cy, tr, NULL);
}

/* ---- input helpers -------------------------------------------------------------------------------------------- */
bool ui_in(Rect r)
{
    int mx = input_mouse_x(), my = input_mouse_y();
    return mx >= r.x0 && mx < r.x1 && my >= r.y0 && my < r.y1;
}

bool ui_lclick(void) { return input_query_mouse(0, INPUT_CODE_CLICK) != 0; }
bool ui_rclick(void) { return input_take_menu_toggle(); }   /* close/back: right click or Escape down, used up */
bool ui_enter_pressed(void)
{
    return input_key_pressed(SDL_SCANCODE_RETURN) || input_key_pressed(SDL_SCANCODE_KP_ENTER);
}

int ui_cycle(int cur, int n, int step)
{
    if (n <= 0) return -1;
    return step > 0 ? (cur + 1) % n : (cur <= 0 ? n : cur) - 1;
}

/* ---- cursor --------------------------------------------------------------------------------------------------- */
/* #MOUSE_CURSOR.000.FILE as an ASCII image name (read once; non-ASCII units are skipped, as before). */
static void cursor_name(char name[CURSOR_NAME_MAX])
{
    UiText u;
    ui_text_gx(&u, "MOUSE_CURSOR.000.FILE", 0, "_mouse_00");
    int k = 0;
    for (int i = 0; i < u.n && k < CURSOR_NAME_MAX - 1; i++)
        if (u.u[i] < 0x80) name[k++] = (char)u.u[i];
    name[k] = 0;
    if (!name[0]) {
        bool ok = str_copy_c(name, CURSOR_NAME_MAX, "_mouse_00");
        ASSERT(ok);
    }
}

void ui_cursor_draw(Canvas *fb)
{
    static char file[CURSOR_NAME_MAX];
    static int speed = CURSOR_SPEED_MS;
    if (!ui_engine_cursor()) return;
    int mx = input_mouse_x(), my = input_mouse_y();
    if (mx < 0 || my < 0 || mx >= SCREEN_W || my >= SCREEN_H) return;
    if (!file[0]) {
        cursor_name(file);
        speed = gx_int("MOUSE_CURSOR.000.SPEED", 0, CURSOR_SPEED_MS);
        if (speed <= 0) speed = CURSOR_SPEED_MS;
    }
    int t = clock_real_ms();
    int pat = (t < 0 ? 0 : t / speed) % CURSOR_CUTS;
    if (!img_get(file, pat)) pat = 0;
    ui_image(fb, file, pat, mx, my, 255);
}
