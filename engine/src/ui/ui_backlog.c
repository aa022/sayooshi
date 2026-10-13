/* ui/ui_backlog.c — backlog viewer (TXT §7: #MSGBK.*, #MSGBK_ITEM.*): wrapped lines of the backlog entries,
 * scrolling by wheel / keys / slider, voice replay buttons.
 *
 * Memory: the wrapped lines are a fixed static ring of BL_LINES_MAX records (entry, offset, length) built when the
 * viewer opens; when the backlog would need more lines the oldest are dropped. Drawing reads the backlog's own
 * strings and translates names into a stack buffer; nothing is allocated. */
#include "ui/ui_internal.h"
#include "core/str.h"
#include "plat/input.h"
#include "txt/backlog.h"
#include "txt/lang.h"
#include "snd/audio.h"
#include "fmt/gameexe.h"
#include <SDL3/SDL.h>

enum {
    BL_LINES_MAX = 16384,       /* 1000 entries x (name + several lines) with room to spare */
    BL_NAME_MAX = 128,          /* units of a displayed speaker name */
    BL_BTN = 20, BL_SLIDER_THUMB = 20, BL_WHEEL_LINES = 3, KOE_FADE_MS = 100,
};

typedef struct { int entry, off, len; bool name, first; } BLine;

static BLine g_line[BL_LINES_MAX];              /* ring: line i is g_line[(g_first + i) % BL_LINES_MAX] */
static int g_first, g_nline;
static struct { int msg_x, size, pitch, top, rows, width; int close_x, close_y, sl_x, sl_y0, sl_y1, koe_dx; } g_mb;

static const BLine *line_at(int i) { return &g_line[(g_first + i) % BL_LINES_MAX]; }

static void line_add(int entry, int off, int len, bool name, bool first)
{
    if (g_nline == BL_LINES_MAX) { g_first = (g_first + 1) % BL_LINES_MAX; g_nline--; }   /* drop the oldest */
    g_line[(g_first + g_nline++) % BL_LINES_MAX] = (BLine){ entry, off, len, name, first };
}

static bool is_word(uint16_t c) { return c >= 0x21 && c <= 0x7E; }

/* One display line of t starting at i: as many units as fit in the width (at least one), up to a '\n'. */
static int line_end(const WStr *t, int i)
{
    int start = i, w = 0;
    while (i < t->len && t->p[i] != '\n') {
        int cw = ui_u16_w(&t->p[i], 1, g_mb.size);
        if (w + cw > g_mb.width && i > start) break;
        w += cw;
        i++;
    }
    return i;
}

/* Japanese: break anywhere. English (SPEC_ENPATCH §1.4 "backlog": the EN exe lays the backlog out with the same
 * word-wrapping routine): a line never ends inside a run of 0x21..0x7e unless the run alone is wider than a line,
 * and the spaces at a break are dropped. Every iteration consumes at least one unit. */
static void wrap_text(int entry, const WStr *t, bool *first)
{
    bool latin = lang_latin_layout();
    int i = 0, n = t->len;
    while (i < n) {
        int start = i, end = line_end(t, i);
        i = end;
        if (latin && i < n && is_word(t->p[i])) {
            int k = i;
            while (k > start && is_word(t->p[k - 1])) k--;
            if (k > start) end = k;                  /* back to the start of the word that did not fit */
        }
        line_add(entry, start, end - start, false, *first);
        *first = false;
        i = end;
        if (i < n && t->p[i] == '\n') i++;
        else if (latin) while (i < n && (t->p[i] == ' ' || t->p[i] == 0x3000)) i++;
    }
}

static void load_layout(void)
{
    g_mb.msg_x = gx_int("MSGBK.MESSAGE_POS", 0, 60);
    g_mb.size = SDL_clamp(gx_int("MSGBK.MOJI_SIZE", 0, 20), 1, 255);
    g_mb.pitch = g_mb.size + gx_int("MSGBK.MOJI_SPACE", 1, 4);
    g_mb.top = gx_int("MSGBK.DISP_MARGIN", 1, 10);
    int bottom = gx_int("MSGBK.DISP_MARGIN", 3, 10), h = gx_int("MSGBK.WINDOW_SIZE", 1, 480);
    g_mb.rows = g_mb.pitch > 0 ? (h - g_mb.top - bottom) / g_mb.pitch : 19;
    g_mb.rows = SDL_clamp(g_mb.rows, 1, SCREEN_H);
    g_mb.width = gx_int("MSGBK.MOJI_CNT", 0, 25) * g_mb.size;
    g_mb.close_x = gx_int("MSGBK_ITEM.CLOSE_BTN.POS", 0, 610);
    g_mb.close_y = gx_int("MSGBK_ITEM.CLOSE_BTN.POS", 1, 20);
    g_mb.sl_x = gx_int("MSGBK_ITEM.SLIDER.POS", 0, 611);
    g_mb.sl_y0 = gx_int("MSGBK_ITEM.SLIDER.POS", 1, 53);
    g_mb.sl_y1 = gx_int("MSGBK_ITEM.SLIDER.POS", 2, 464);       /* [I] bottom of the track (the back image's frame) */
    g_mb.koe_dx = gx_int("MSGBK_ITEM.KOE_BTN.POS", 0, -25);
}

void ui_backlog_open(UiScreen *s)
{
    load_layout();
    g_first = g_nline = 0;
    int n = backlog_count();
    for (int e = 0; e < n; e++) {
        const BacklogEntry *be = backlog_get(e);
        if (!be) continue;
        bool first = true;
        if (be->name.len > 0) { line_add(e, 0, be->name.len, true, true); first = false; }
        if (be->text.len > 0) wrap_text(e, &be->text, &first);
        else if (first) line_add(e, 0, 0, false, true);
    }
    s->scroll = g_nline > g_mb.rows ? g_nline - g_mb.rows : 0;
    s->drag = 0;
}

static int max_scroll(void) { return g_nline > g_mb.rows ? g_nline - g_mb.rows : 0; }
static int slider_range(void) { return g_mb.sl_y1 - BL_SLIDER_THUMB - g_mb.sl_y0; }
static Rect close_rect(void)
{
    return (Rect){ g_mb.close_x - 4, g_mb.close_y - 4, g_mb.close_x + 19, g_mb.close_y + 19 };
}

static int thumb_y(const UiScreen *s)
{
    int range = slider_range(), m = max_scroll();
    return g_mb.sl_y0 + (m > 0 ? range * s->scroll / m : range);
}

static void scroll_keys(UiScreen *s, int m)
{
    if (input_key_pressed(SDL_SCANCODE_UP)) s->scroll--;
    if (input_key_pressed(SDL_SCANCODE_DOWN)) s->scroll++;
    if (input_key_pressed(SDL_SCANCODE_PAGEUP)) s->scroll -= g_mb.rows;
    if (input_key_pressed(SDL_SCANCODE_PAGEDOWN)) s->scroll += g_mb.rows;
    if (input_key_pressed(SDL_SCANCODE_HOME)) s->scroll = 0;
    if (input_key_pressed(SDL_SCANCODE_END)) s->scroll = m;
}

static void slider_drag(UiScreen *s, int m)
{
    const InputBtn *l = input_btn_mouse(0);
    if (l->on_down && ui_in((Rect){ g_mb.sl_x - 6, g_mb.sl_y0, g_mb.sl_x + BL_BTN, g_mb.sl_y1 })) s->drag = 1;
    if (!s->drag) return;
    int range = slider_range();
    if (range > 0) s->scroll = (input_mouse_y() - BL_SLIDER_THUMB / 2 - g_mb.sl_y0) * m / range;
    if (!l->is_down) s->drag = 0;
}

/* Voice replay buttons next to the first line of entries with a koe. */
static void koe_buttons(UiScreen *s)
{
    s->hover = -1;
    for (int r = 0; r < g_mb.rows && s->scroll + r < g_nline; r++) {
        const BLine *l = line_at(s->scroll + r);
        const BacklogEntry *be = backlog_get(l->entry);
        if (!l->first || !be || be->koe_no < 0) continue;
        int x = g_mb.msg_x + g_mb.koe_dx, y = g_mb.top + r * g_mb.pitch;
        if (!ui_in((Rect){ x, y, x + BL_BTN, y + BL_BTN - 1 })) continue;
        s->hover = s->scroll + r;
        if (ui_lclick()) koe_play(be->koe_no, be->chara, KOE_FADE_MS);
    }
}

void ui_backlog_frame(UiScreen *s)
{
    int m = max_scroll(), wheel = input_wheel();
    if (ui_rclick() || (ui_lclick() && ui_in(close_rect()))) { ui_pop(); return; }
    if (wheel < 0 && s->scroll >= m) { ui_pop(); return; }     /* wheel-down past the newest line closes */
    s->scroll -= wheel * BL_WHEEL_LINES;
    scroll_keys(s, m);
    slider_drag(s, m);
    s->scroll = SDL_clamp(s->scroll, 0, m);
    koe_buttons(s);
}

/* ＊Ａ etc. expanded (disp_name); English: the translated name, Japanese: in 【 】. */
static void draw_name(Canvas *fb, const BacklogEntry *be, int y)
{
    uint16_t mem[BL_NAME_MAX + 1];
    WBuf dn;
    wb_init(&dn, mem, COUNT_OF(mem));
    lang_display_name_buf(be->disp_name.len > 0 ? &be->disp_name : &be->name, &dn);
    int x = g_mb.msg_x, n = (int)dn.n;
    if (lang_is_en()) {
        ui_draw_u16(fb, x, y, g_mb.size, UI_COL_BACKLOG_NAME, mem, n);
        return;
    }
    static const uint16_t lb = 0x3010, rb = 0x3011;   /* 【 】 */
    ui_draw_u16(fb, x, y, g_mb.size, UI_COL_TEXT, &lb, 1);
    x += g_mb.size;
    ui_draw_u16(fb, x, y, g_mb.size, UI_COL_TEXT, mem, n);
    x += ui_u16_w(mem, n, g_mb.size);
    ui_draw_u16(fb, x, y, g_mb.size, UI_COL_TEXT, &rb, 1);
}

static void draw_image_or_box(Canvas *fb, const char *img, int x, int y, Rect box, int tr)
{
    if (img_get(img, 0)) ui_image(fb, img, 0, x, y, tr);
    else ui_fill(fb, box, UI_COL_WHITE, tr);
}

static void draw_line(Canvas *fb, const UiScreen *s, int r)
{
    const BLine *l = line_at(s->scroll + r);
    const BacklogEntry *be = backlog_get(l->entry);
    if (!be) return;
    int y = g_mb.top + r * g_mb.pitch;
    if (l->name) draw_name(fb, be, y);
    else if (l->len > 0 && l->off + l->len <= be->text.len)
        ui_draw_u16(fb, g_mb.msg_x, y, g_mb.size, UI_COL_TEXT, be->text.p + l->off, l->len);
    if (l->first && be->koe_no >= 0) {        /* #MSGBK_ITEM.KOE_BTN, action 5: 180 normal / 255 hit */
        int x = g_mb.msg_x + g_mb.koe_dx, tr = s->hover == s->scroll + r ? 255 : 180;
        draw_image_or_box(fb, "_fmsgbk_btn_koe", x, y, (Rect){ x + 4, y + 4, x + 16, y + 16 }, tr);
    }
}

void ui_backlog_draw(Canvas *fb, const UiScreen *s)
{
    if (img_get("_fmsgbk_back", 0)) ui_image(fb, "_fmsgbk_back", 0, 0, 0, 255);
    else ui_fill(fb, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, UI_COL_BACKLOG_SHADE, 230);
    for (int r = 0; r < g_mb.rows && s->scroll + r < g_nline; r++) draw_line(fb, s, r);
    int cx = g_mb.close_x, cy = g_mb.close_y, ctr = ui_in(close_rect()) ? 255 : 180;
    draw_image_or_box(fb, "_fmsgbk_btn_close", cx, cy, (Rect){ cx, cy, cx + 15, cy + 15 }, ctr);
    int ty = thumb_y(s);
    draw_image_or_box(fb, "_fmsgbk_slider", g_mb.sl_x, ty, (Rect){ g_mb.sl_x, ty, g_mb.sl_x + 13, ty + 20 }, 255);
}
