/* txt/mwnd_draw.c — message window drawing: filter rectangle, glyph layers, choices, key icon (TXT §5.1, §1.6),
 * and the style it uses: #COLOR_TABLE, #MWND colours, #CONFIG.FONT, #ICON (AST §6).
 *
 * Memory: fixed static tables only; drawing never allocates (glyph bitmaps come from the font cache). */
#include "txt/mwnd.h"
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/font.h"
#include "fmt/gameexe.h"

enum {
    COLOR_TABLE_CNT = 256, COLOR_HOVER = 5,          /* COLOR_TABLE[5] = choice under the cursor (ENGINE_SPEC §1.6) */
    ICON_CNT = 16, ICON_FILE_MAX = 64, ICON_SPEED_DEFAULT = 160,
};

typedef struct { char file[ICON_FILE_MAX]; int speed, cnt; } IconDef;

static uint32_t g_color_table[COLOR_TABLE_CNT];
static IconDef g_icon[ICON_CNT];
static int g_shadow_mode = 2, g_moji_col = 0, g_fuchi_col = 1, g_shadow_col = 1;
static bool g_futoku;

int mwnd_style_moji_col(void) { return g_moji_col; }
int mwnd_style_fuchi_col(void) { return g_fuchi_col; }
int mwnd_style_shadow_col(void) { return g_shadow_col; }
void mwnd_set_font_style(int shadow_mode, bool futoku) { g_shadow_mode = shadow_mode; g_futoku = futoku; }

static void load_color_table(void)
{
    for (int i = 0; i < COLOR_TABLE_CNT; i++) {
        char k[TXT_GX_KEY_MAX];
        bool fits = str_fmt(k, sizeof k, "COLOR_TABLE.%03d", i);
        ASSERT(fits);
        g_color_table[i] = (uint32_t)(gx_int(k, 0, 0) & 255) << 16 | (uint32_t)(gx_int(k, 1, 0) & 255) << 8 |
                           (uint32_t)(gx_int(k, 2, 0) & 255);
    }
    if (!gx_has("COLOR_TABLE.000")) { g_color_table[0] = 0xFFFFFF; g_color_table[COLOR_HOVER] = 0xFFFF00; }
}

/* #ICON.n.FILE into d->file; a name that does not fit is dropped (logged), never cut. */
static void load_icon_file(IconDef *d, int no)
{
    char k[TXT_GX_KEY_MAX];
    txt_gx_key(k, "ICON", no, "FILE");
    d->file[0] = 0;
    WStr f = {0};
    if (gx_str(k, 0, &f)) {
        StrBuf b;
        sb_init(&b, d->file, sizeof d->file);
        if (!ws_to_utf8(&f, &b) || !sb_ok(&b)) {
            log_msg(LOG_WARN, "mwnd: %s is longer than %d bytes; icon %d disabled", k, ICON_FILE_MAX - 1, no);
            d->file[0] = 0;
        }
    }
    ws_free(&f);
}

static void load_icons(void)
{
    for (int i = 0; i < ICON_CNT; i++) {
        load_icon_file(&g_icon[i], i);
        g_icon[i].speed = txt_gx_int("ICON", i, "SPEED", 0, ICON_SPEED_DEFAULT);
        g_icon[i].cnt = txt_gx_int("ICON", i, "CNT", 0, 1);
    }
    if (!g_icon[0].file[0] && !gx_has("ICON.000.FILE")) {           /* AST §6 values if Gameexe lacks them */
        bool ok = str_copy_c(g_icon[0].file, ICON_FILE_MAX, "_key_r") &&
                  str_copy_c(g_icon[1].file, ICON_FILE_MAX, "_key_page");
        ASSERT(ok);
        g_icon[0].cnt = 8;
        g_icon[1].cnt = 8;
    }
}

void mwnd_style_load(void)
{
    load_color_table();
    g_moji_col = gx_int("MWND.MOJI_COLOR", 0, 0) & 255;
    g_fuchi_col = gx_int("MWND.FUCHI_COLOR", 0, 1) & 255;
    g_shadow_col = gx_int("MWND.SHADOW_COLOR", 0, 1) & 255;
    load_icons();
}

/* Straight colour rgb at coverage a over the clipped rectangle. */
static void fill_rect(Canvas *c, Rect r, uint32_t rgb, int a)
{
    if (!c || !c->px || a <= 0) return;
    if (r.x0 < 0) r.x0 = 0;
    if (r.y0 < 0) r.y0 = 0;
    if (r.x1 > c->w) r.x1 = c->w;
    if (r.y1 > c->h) r.y1 = c->h;
    for (int y = r.y0; y < r.y1; y++) {
        uint32_t *row = c->px + (size_t)y * c->pitch;
        for (int x = r.x0; x < r.x1; x++) row[x] = txt_px_over(row[x], rgb, a);
    }
}

static void draw_glyph(Canvas *fb, const MGlyph *g, int ox, int oy, uint32_t moji, int tr)
{
    font_draw_char(fb, g->code, g->size, ox + g->x, oy + g->y, moji, g_color_table[g->fuchi],
                   g_color_table[g->shadow], g_shadow_mode, g_futoku, tr);
}

/* §1.6: anime type 1 = linear fade of the whole window. */
static int window_alpha(const Mwnd *w)
{
    int tr = 255;
    if (w->open_anim >= 0 && w->cfg.open_time > 0) tr = 255 * w->open_anim / w->cfg.open_time;
    if (w->close_anim >= 0 && w->cfg.close_time > 0) tr = 255 - 255 * w->close_anim / w->cfg.close_time;
    return tr;
}

static void draw_filter(Canvas *fb, const Mwnd *w, int tr)
{
    const MwndCfg *c = &w->cfg;
    Rect r = { c->win_x + w->filter_margin[0], c->win_y + w->filter_margin[1],
               c->win_x + c->win_w - w->filter_margin[2], c->win_y + c->win_h - w->filter_margin[3] };
    uint32_t rgb = (uint32_t)w->filter[0] << 16 | (uint32_t)w->filter[1] << 8 | w->filter[2];
    fill_rect(fb, r, rgb, w->filter[3] * tr / 255);
}

static void draw_choices(Canvas *fb, const Mwnd *w, int ox, int oy, int tr)
{
    for (int k = 0; k < w->nsel; k++) {
        const SelItem *it = &w->sel[k];
        bool hot = w->sel_active && w->sel_hover == k;
        for (int j = it->g0; j < it->g1 && j < w->nsel_g; j++) {
            uint32_t col = hot ? g_color_table[COLOR_HOVER] : g_color_table[w->sel_g[j].moji];
            draw_glyph(fb, &w->sel_g[j], ox, oy, col, tr);
        }
    }
}

/* TXT §5.1: the key icon after the last glyph, only during proc 0x1a. */
static void draw_key_icon(Canvas *fb, const Mwnd *w, int ox, int oy, int tr)
{
    int n = w->page_icon ? w->page_icon_no : w->icon_no;
    if (n < 0 || n >= ICON_CNT || !g_icon[n].file[0]) return;
    const IconDef *d = &g_icon[n];
    int frame = d->cnt > 0 && d->speed > 0 ? (int)((uint32_t)w->icon_time / (uint32_t)d->speed) % d->cnt : 0;
    const Image *img = img_get(d->file, frame);
    if (img) canvas_blit(fb, img, (Rect){0, 0, img->w, img->h}, ox + w->msg.x, oy + w->msg.y, tr, NULL);
}

static void draw_window(Canvas *fb, const Mwnd *w)
{
    int tr = window_alpha(w);
    draw_filter(fb, w, tr);
    int ox = w->cfg.win_x + w->cfg.msg_x, oy = w->cfg.win_y + w->cfg.msg_y;
    for (int k = 0; k < w->msg.shown && k < w->msg.ng; k++)
        draw_glyph(fb, &w->msg.g[k], ox, oy, g_color_table[w->msg.g[k].moji], tr);
    draw_choices(fb, w, ox, oy, tr);
    if (w->key_wait) draw_key_icon(fb, w, ox, oy, tr);
}

void mwnd_draw(Canvas *fb)
{
    if (mwnd_user_hidden()) return;
    for (int i = 0; i < MWND_CNT; i++) {
        const Mwnd *w = mwnd_get(i);
        if (w->open || w->close_anim >= 0) draw_window(fb, w);
    }
}
