/* ui/ui_saveload.c — save/load slot dialog (ENGINE_SPEC §2.12; #SAVE.CNT = 100,
 * #SAVELOAD_DIALOG.DATA_CNT_PAR_PAGE = 10; SAVE_FORMAT.md §5 for why a slot cannot be loaded).
 *
 * Memory: the shown page's SavInfo records (released with sav_info_free) and their thumbnails converted to
 * premultiplied ARGB on the tracked heap (MEM_UI). Both live while the page is shown: ui_saveload_release runs on a
 * page change, when the dialog closes and at ui_shutdown. */
#include "ui/ui_internal.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "plat/input.h"
#include "sav/sav.h"
#include "fmt/gameexe.h"
#include "txt/lang.h"
#include <SDL3/SDL.h>
#include <string.h>

enum {
    SL_ROWS_MAX = 10, SL_PAGES_MAX = 10, SL_X0 = 30, SL_X1 = 610, SL_Y0 = 52, SL_ROW = 38,
    SL_TAB_Y = 440, SL_TAB_H = 28, SL_BACK_W = 60, SL_TEXT = 16, SL_TAB_TEXT = 18,
    SL_TEXT_X = SL_X0 + 78,              /* slot number and message column (right of the thumbnail) */
    SL_DATE_X = SL_X0 + 150, SL_TITLE_X = SL_X0 + 320,
    SL_LINE1_DY = 2, SL_LINE2_DY = 19,   /* the two text lines of a row */
    SL_TEXT_PAD = 6,                     /* right margin of the clipped texts */
    SL_TAG_GAP = 8,                      /* after the [EN] / [JA] tag */
    SL_CAPTION_Y = 14, SL_CAPTION_TEXT = 24, SL_BACK_TEXT_DX = 50,
    SL_THUMB_W = 64, SL_THUMB_H = 34, THUMB_DIM_MAX = 1024,
    NOTE_MAX = 256,
};

static int g_page, g_rows = SL_ROWS_MAX, g_pages = SL_PAGES_MAX;
static SavInfo g_info[SL_ROWS_MAX];
static bool g_used[SL_ROWS_MAX];
static Image g_thumb[SL_ROWS_MAX];
static struct { UiText cap_save, cap_load, warn_save, warn_load, nodata, back; } g_txt;

void ui_saveload_release(void)
{
    for (int i = 0; i < SL_ROWS_MAX; i++) {
        if (g_used[i]) sav_info_free(&g_info[i]);
        g_used[i] = false;
        mem_free(g_thumb[i].px);
        memset(&g_thumb[i], 0, sizeof g_thumb[i]);
    }
}

/* RGB888 thumbnail -> opaque premultiplied ARGB image on the heap; empty image for missing or absurd sizes. */
static Image thumb_image(const SavInfo *si)
{
    int tw = si->thumb_w, th = si->thumb_h;
    if (!si->thumb_rgb || tw <= 0 || th <= 0 || tw > THUMB_DIM_MAX || th > THUMB_DIM_MAX) return (Image){ 0 };
    size_t n = (size_t)tw * (size_t)th;
    uint32_t *px = mem_alloc_array(n, sizeof *px, MEM_UI);
    const uint8_t *rgb = si->thumb_rgb;
    for (size_t k = 0; k < n; k++)
        px[k] = 0xFF000000u | (uint32_t)rgb[k * 3] << 16 | (uint32_t)rgb[k * 3 + 1] << 8 | rgb[k * 3 + 2];
    return (Image){ px, tw, th, 0, 0 };
}

static void refresh(int page)
{
    ui_saveload_release();
    for (int i = 0; i < g_rows; i++) {
        SavInfo *si = &g_info[i];
        memset(si, 0, sizeof *si);
        if (!sav_slot_info(page * g_rows + i, si) || !si->used) {
            if (si->used) sav_info_free(si);
            continue;
        }
        g_used[i] = true;
        g_thumb[i] = thumb_image(si);
    }
}

/* Re-read on every open: the language (and with it Gameexe) can change at run time. */
static void load_labels(void)
{
    ui_text_gx(&g_txt.cap_save, "SAVEINFO.DLGWND_CAPTION_TITLE_STR", 0, lang_str(LANG_STR_SAVE));
    ui_text_gx(&g_txt.cap_load, "LOADINFO.DLGWND_CAPTION_TITLE_STR", 0, lang_str(LANG_STR_LOAD));
    ui_text_gx(&g_txt.warn_save, "SAVEINFO.WARNING_STR", 0, lang_str(LANG_STR_SAVE_WARN));
    ui_text_gx(&g_txt.warn_load, "LOADINFO.WARNING_STR", 0, lang_str(LANG_STR_LOAD_WARN));
    ui_text_gx(&g_txt.nodata, "SAVE.NO_DATA_STR", 0, lang_str(LANG_STR_NO_DATA));
    ui_text_set(&g_txt.back, lang_str(LANG_STR_BACK));
    g_rows = gx_int("SAVELOAD_DIALOG.DATA_CNT_PAR_PAGE", 0, SL_ROWS_MAX);
    if (g_rows < 1 || g_rows > SL_ROWS_MAX) g_rows = SL_ROWS_MAX;
    int cnt = gx_int("SAVE.CNT", 0, SAV_SLOT_CNT);
    if (cnt < 1) cnt = 1;
    if (cnt > SL_ROWS_MAX * SL_PAGES_MAX) cnt = SL_ROWS_MAX * SL_PAGES_MAX;
    g_pages = (cnt + g_rows - 1) / g_rows;
    if (g_pages > SL_PAGES_MAX) g_pages = SL_PAGES_MAX;
}

void ui_saveload_open(UiScreen *s)
{
    load_labels();
    if (g_page >= g_pages) g_page = 0;
    s->page = g_page;
    refresh(s->page);
}

static Rect row_rect(int i) { return (Rect){ SL_X0, SL_Y0 + i * SL_ROW, SL_X1, SL_Y0 + i * SL_ROW + SL_ROW - 2 }; }
static Rect back_rect(void) { return (Rect){ SL_X1 - SL_BACK_W, SL_TAB_Y, SL_X1, SL_TAB_Y + SL_TAB_H }; }

static Rect tab_rect(int k)
{
    int w = (SL_X1 - SL_BACK_W - 10 - SL_X0) / g_pages, x0 = SL_X0 + k * w;
    return (Rect){ x0, SL_TAB_Y, x0 + w - 4, SL_TAB_Y + SL_TAB_H };
}

static void set_page(UiScreen *s, int page)
{
    page = (page % g_pages + g_pages) % g_pages;
    if (page == s->page) return;
    s->page = g_page = page;
    refresh(page);
}

static void activate(const UiScreen *s, int row)
{
    int slot = s->page * g_rows + row;
    if (s->kind == UI_SAVE) {
        if (g_used[row]) ui_confirm(&g_txt.warn_save, UIA_SAVE, slot);
        else ui_do_action(UIA_SAVE, slot);
    } else if (g_used[row] && sav_info_loadable(&g_info[row])) {   /* saves from other scene code are refused */
        ui_confirm(&g_txt.warn_load, UIA_LOAD, slot);
    }
}

static void close_dialog(void)
{
    ui_saveload_release();
    ui_pop();
}

static void click(UiScreen *s, int hit)
{
    if (ui_in(back_rect())) { close_dialog(); return; }
    for (int k = 0; k < g_pages; k++)
        if (ui_in(tab_rect(k))) set_page(s, k);
    if (hit >= 0) activate(s, hit);
}

void ui_saveload_frame(UiScreen *s)
{
    int hit = -1;
    for (int i = 0; i < g_rows; i++)
        if (ui_in(row_rect(i))) hit = s->hover = i;
    if (input_key_pressed(SDL_SCANCODE_DOWN)) s->hover = ui_cycle(s->hover, g_rows, 1);
    if (input_key_pressed(SDL_SCANCODE_UP)) s->hover = ui_cycle(s->hover, g_rows, -1);
    if (input_key_pressed(SDL_SCANCODE_RIGHT) || input_wheel() < 0) set_page(s, s->page + 1);
    if (input_key_pressed(SDL_SCANCODE_LEFT) || input_wheel() > 0) set_page(s, s->page - 1);
    if (ui_rclick()) { close_dialog(); return; }
    if (ui_lclick()) {
        click(s, hit);                                  /* may close the dialog or open a confirm box */
    } else if (ui_enter_pressed() && s->hover >= 0) {
        activate(s, s->hover);
    }
    if (!ui_active()) ui_saveload_release();            /* the action closed every screen */
}

/* ---- drawing ------------------------------------------------------------------------------------------------- */
/* days since 1970-01-01 -> civil date (H. Hinnant's algorithm), UTC */
static void civil(int64_t t, int *y, int *mo, int *d, int *h, int *mi)
{
    int64_t days = t >= 0 ? t / 86400 : (t - 86399) / 86400, sec = t - days * 86400;
    *h = (int)(sec / 3600); *mi = (int)(sec / 60 % 60);
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097, doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365, doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1);
    *mo = (int)(mp < 10 ? mp + 3 : mp - 9);
    *y = (int)(yoe + era * 400 + (*mo <= 2));
}

/* "YYYY/MM/DD hh:mm" in local time (UTC when the clock value is out of SDL's range). */
static void format_time(UiText *out, int64_t unix_time)
{
    int yy, mo, d, h, mi;
    SDL_DateTime dt;
    if (unix_time > 0 && unix_time < INT64_C(9000000000) &&
        SDL_TimeToDateTime((SDL_Time)unix_time * SDL_NS_PER_SECOND, &dt, true)) {
        yy = dt.year; mo = dt.month; d = dt.day; h = dt.hour; mi = dt.minute;
    } else {
        civil(unix_time, &yy, &mo, &d, &h, &mi);
    }
    SB_ON_STACK(b, 48);
    (void)sb_fmt(&b, "%04d/%02d/%02d %02d:%02d", yy, mo, d, h, mi);   /* <= 40 bytes for any int values */
    ui_text_set(out, sb_cstr(&b));
}

/* A lang.h template with "%s" placeholders filled in order (never used as a printf format). */
static void fill_template(StrBuf *out, const char *tmpl, const char *a, const char *b)
{
    int arg = 0;
    for (const char *p = tmpl; *p; p++) {
        if (p[0] == '%' && p[1] == 's') {
            (void)sb_put_c(out, arg++ == 0 ? a : b);    /* truncation is sticky and visible in the label */
            p++;
        } else {
            (void)sb_put_char(out, *p);
        }
    }
}

/* Why a slot cannot be loaded, in the current language (SAVE_FORMAT.md §5: the call-stack scene code must match). */
static void slot_note(const SavInfo *si, UiText *out)
{
    LangId made = lang_parse(si->lang);
    char mem[NOTE_MAX];
    StrBuf b;
    sb_init(&b, mem, sizeof mem);
    switch (si->state) {
    case SAV_SLOT_OLD_FORMAT: (void)sb_put_c(&b, lang_str(LANG_STR_SLOT_OLD_FORMAT)); break;
    case SAV_SLOT_CORRUPT:    (void)sb_put_c(&b, lang_str(LANG_STR_SLOT_DAMAGED)); break;
    case SAV_SLOT_OTHER_LANG:
        fill_template(&b, lang_str(LANG_STR_SLOT_OTHER_LANG),
                      lang_str(made == LANG_EN ? LANG_STR_SLOT_LANG_TAG_EN : LANG_STR_SLOT_LANG_TAG_JA),
                      lang_native_name(made));
        break;
    default:                  (void)sb_put_c(&b, lang_str(LANG_STR_SLOT_OTHER_DATA)); break;
    }
    ui_text_set(out, sb_cstr(&b));   /* a cut note is still drawn (truncation is visible) */
}

static void draw_slot_details(Canvas *fb, const SavInfo *si, int y, bool enabled, uint32_t col)
{
    UiText u;
    format_time(&u, si->unix_time);
    ui_draw_text(fb, SL_DATE_X, y + SL_LINE1_DY, SL_TEXT, col, &u);
    int tx = SL_TITLE_X;
    if (si->lang[0] && strcmp(si->lang, lang_code()) != 0) {   /* made in the other language: tag it */
        ui_text_set(&u, lang_parse(si->lang) == LANG_EN ? "[EN]" : "[JA]");
        ui_draw_text(fb, tx, y + SL_LINE1_DY, SL_TEXT, enabled ? UI_COL_LANG_TAG : UI_COL_TEXT_OFF, &u);
        tx += ui_text_w(&u, SL_TEXT) + SL_TAG_GAP;
    }
    if (si->title.p)
        ui_draw_clipped(fb, tx, y + SL_LINE1_DY, SL_TEXT, col, si->title.p, si->title.len, SL_X1 - tx - SL_TEXT_PAD);
    int msg_w = SL_X1 - SL_TEXT_X - SL_TEXT_PAD;
    if (!sav_info_loadable(si)) {            /* e.g. made with the other language's differing scene code */
        slot_note(si, &u);
        ui_draw_clipped(fb, SL_TEXT_X, y + SL_LINE2_DY, SL_TEXT, UI_COL_REFUSED, u.u, u.n, msg_w);
    } else if (si->message.p) {
        ui_draw_clipped(fb, SL_TEXT_X, y + SL_LINE2_DY, SL_TEXT, UI_COL_MESSAGE, si->message.p, si->message.len, msg_w);
    }
}

static void draw_row(Canvas *fb, const UiScreen *s, int i)
{
    int y = SL_Y0 + i * SL_ROW, slot = s->page * g_rows + i;
    bool enabled = s->kind == UI_SAVE || (g_used[i] && sav_info_loadable(&g_info[i]));
    ui_fill(fb, row_rect(i), i == s->hover ? UI_COL_ROW_HOVER : UI_COL_ROW, 220);
    ui_frame_rect(fb, (Rect){ SL_X0 + 2, y + 1, SL_X0 + 70, y + 37 }, UI_COL_THUMB_FRAME);
    const Image *th = &g_thumb[i];
    if (th->px)
        canvas_blit_scaled(fb, th, (Rect){ 0, 0, th->w, th->h }, (float)(SL_X0 + 4), (float)(y + 2),
                           (float)SL_THUMB_W / (float)th->w, (float)SL_THUMB_H / (float)th->h,
                           enabled ? 255 : 110, NULL);
    char no[16];
    bool fits = str_fmt(no, sizeof no, "No.%03d", slot + 1);   /* slot < 100 */
    ASSERT(fits);
    UiText u;
    ui_text_set(&u, no);
    uint32_t col = !enabled ? UI_COL_TEXT_OFF : i == s->hover ? UI_COL_TEXT_HOVER : UI_COL_TEXT;
    ui_draw_text(fb, SL_TEXT_X, y + SL_LINE1_DY, SL_TEXT, col, &u);
    if (g_used[i]) draw_slot_details(fb, &g_info[i], y, enabled, col);
    else ui_draw_text(fb, SL_TEXT_X, y + SL_LINE2_DY, SL_TEXT, UI_COL_NO_DATA, &g_txt.nodata);
}

static void draw_tabs(Canvas *fb, const UiScreen *s)
{
    for (int k = 0; k < g_pages; k++) {
        Rect r = tab_rect(k);
        char num[8];
        bool fits = str_fmt(num, sizeof num, "%d", k + 1);      /* k < SL_PAGES_MAX */
        ASSERT(fits);
        UiText u;
        ui_text_set(&u, num);
        ui_fill(fb, r, k == s->page ? UI_COL_HILITE : UI_COL_TAB, 230);
        int x = r.x0 + (r.x1 - r.x0 - ui_text_w(&u, SL_TAB_TEXT)) / 2;
        ui_draw_text(fb, x, SL_TAB_Y + 5, SL_TAB_TEXT, UI_COL_TEXT, &u);
    }
    ui_fill(fb, back_rect(), UI_COL_BACK, 230);
    ui_draw_text(fb, SL_X1 - SL_BACK_TEXT_DX, SL_TAB_Y + 5, SL_TAB_TEXT, UI_COL_TEXT, &g_txt.back);
}

void ui_saveload_draw(Canvas *fb, const UiScreen *s)
{
    ui_fill(fb, (Rect){ 0, 0, SCREEN_W, SCREEN_H }, UI_COL_BACKDROP, 235);
    ui_draw_text(fb, SL_X0, SL_CAPTION_Y, SL_CAPTION_TEXT, UI_COL_TEXT,
                 s->kind == UI_SAVE ? &g_txt.cap_save : &g_txt.cap_load);
    for (int i = 0; i < g_rows; i++) draw_row(fb, s, i);
    draw_tabs(fb, s);
}
