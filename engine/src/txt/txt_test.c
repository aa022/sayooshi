/* txt/txt_test.c — txt module unit tests (ENGINE_SPEC §5 WP4 self-checks): widths, wrap, kinsoku, talk indent,
 * overflow, sizes, name expansion, typing, replay, backlog, glyphs, English layout and choices, the fixed-capacity
 * stores and font file validation.
 *
 * Memory: the test window and glyph stores are static; strings are freed before returning. */
#include "txt/mwnd.h"
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/backlog.h"
#include "txt/font.h"
#include "txt/lang.h"
#include "plat/fs.h"
#include "vm/sysstate.h"
#include <string.h>

static int g_fails;
#define CHECK(cond) \
    do { if (!(cond)) { log_msg(LOG_ERR, "text_selftest: line %d: %s", __LINE__, #cond); g_fails++; } } while (0)

static Mwnd g_tw;                                   /* test window (not one of the game's) */
static MGlyph g_store[MSG_GLYPH_MAX], g_ref_store[MSG_GLYPH_MAX];

static const MwndCfg JA_CFG = { .novel_mode = 1, .win_w = 640, .win_h = 480, .msg_x = 10, .msg_y = 20, .cols = 26,
                                .rows = 16, .size = 24, .space_x = -1, .space_y = 4, .open_type = 1,
                                .open_time = 250, .close_type = 1, .close_time = 250 };

static void rep(WStr *s, uint16_t c, int n) { for (int i = 0; i < n; i++) ws_push(s, c); }
static void put_ascii(WStr *s, const char *a) { for (; *a; a++) ws_push(s, (uint16_t)(unsigned char)*a); }
static int layout_ws(Msg *m, const WStr *s) { return msg_layout(m, s->p, s->len); }

static void msg_fresh(Msg *m, const MwndCfg *cfg, MGlyph *store)
{
    memset(m, 0, sizeof *m);
    msg_bind(m, store, MSG_GLYPH_MAX);
    msg_config(m, cfg);
    msg_reset(m);
}

static WStr g_name_a, g_name_b, g_name_xy, g_name_loc;   /* the name banks of test_names */

static const WStr *test_resolver(bool global, int idx)
{
    if (!global) return idx == 0 ? &g_name_loc : NULL;
    return idx == 0 ? &g_name_a : idx == 1 ? &g_name_b : idx == 27 ? &g_name_xy : NULL;
}

static bool expand_eq(const char *in, const char *want)
{
    WStr i = {0}, o = {0}, w = {0};
    ws_set_utf8(&i, in); ws_set_utf8(&w, want);
    msg_expand_names(&i, &o, test_resolver);
    bool ok = ws_cmp(&o, &w) == 0;
    ws_free(&i); ws_free(&o); ws_free(&w);
    return ok;
}

/* ---- Japanese layout ---------------------------------------------------------------------------------------- */
static void test_widths(const Msg *m)
{
    CHECK(msg_char_width('A', 24) == 12);
    CHECK(msg_char_width(0x3042, 24) == 24);
    CHECK(msg_char_width(0xFF71, 24) == 12);
    CHECK(m->limit_x == 599 && m->limit_y == 444);   /* cols*size + (cols-1)*space (SPEC_ENPATCH §1.3.2) */
    CHECK(msg_no_line_start(0x3002) && msg_no_line_start(0x300D) && msg_no_line_start(0x3063) &&
          msg_no_line_start(0x2026) && msg_no_line_start('!') && !msg_no_line_start(0x3042) &&
          !msg_no_line_start(0x2015) && !msg_no_line_start(0x300C));
}

static void test_wrap(Msg *m, WStr *s)
{
    /* 26 columns at 23 px pitch; the 27th char wraps */
    ws_set(s, NULL, 0); rep(s, 0x3042, 27);
    CHECK(layout_ws(m, s) == 27 && m->ng == 27);
    CHECK(m->g[25].x == 575 && m->g[25].y == 0);
    CHECK(m->g[26].x == 0 && m->g[26].y == 28);
    /* kinsoku: 。 hangs into the margin; a second one hits the hard limit and wraps */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 0x3042, 26); ws_push(s, 0x3002); ws_push(s, 0x3002);
    layout_ws(m, s);
    CHECK(m->ng == 28 && m->g[26].x == 598 && m->g[26].y == 0);
    CHECK(m->g[27].x == 0 && m->g[27].y == 28);
    /* a space that wraps is dropped */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 0x3042, 26); ws_push(s, 0x3000); ws_push(s, 0x3044);
    layout_ws(m, s);
    CHECK(m->ng == 27 && m->g[26].code == 0x3044 && m->g[26].x == 0 && m->g[26].y == 28);
}

static void test_indent(Msg *m, WStr *s)
{
    /* hanging indent after 「 at the line start */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x300C); rep(s, 0x3042, 30);
    layout_ws(m, s);
    CHECK(m->g[26].x == 23 && m->g[26].y == 28);
    /* the closer resets the indent */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x300C); ws_push(s, 0x3042); ws_push(s, 0x300D); rep(s, 0x3044, 30);
    layout_ws(m, s);
    CHECK(m->g[26].x == 0 && m->g[26].y == 28);
    /* 「 not at the line start does not indent */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x3042); ws_push(s, 0x300C); rep(s, 0x3044, 30);
    layout_ws(m, s);
    CHECK(m->g[26].x == 0);
    /* an explicit newline ends the indent */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x300C); ws_push(s, 0x3042);
    layout_ws(m, s);
    msg_newline(m);
    CHECK(m->x == 0 && m->y == 28 && m->indent_x == 0 && m->at_line_start);
    /* a nested identical opener keeps the indent until the matching closer */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x300E); ws_push(s, 0x300E); ws_push(s, 0x300F); rep(s, 0x3042, 30);
    layout_ws(m, s);
    CHECK(m->g[26].x == 23);
}

static void test_overflow_size(Msg *m, WStr *s)
{
    /* overflow: 16 rows × 26 cols, the rest is the remainder */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 0x3042, 16 * 26 + 5);
    CHECK(layout_ws(m, s) == 16 * 26 && m->ng == 16 * 26);
    CHECK(m->g[16 * 26 - 1].y == 15 * 28);
    CHECK(!msg_fits_lines(m, 1));
    /* size (§1.4.2): smaller glyphs, pitch follows the current size, limits stay; kept across a page clear */
    msg_reset(m);
    msg_set_size(m, 18);
    ws_set(s, NULL, 0); rep(s, 0x3042, 40);
    layout_ws(m, s);
    CHECK(m->g[1].x == 17 && m->g[34].y == 0 && m->g[35].y == 22);   /* 35 × 17 px = 595 ≤ 598 */
    msg_reset(m);
    CHECK(m->size == 18);
    msg_set_size(m, -1);
    CHECK(m->size == 24);
    msg_set_size(m, 100000);
    CHECK(m->size == MSG_SIZE_MAX);                  /* hostile script sizes are clamped */
    msg_set_size(m, -1);
    /* ＃Ａ１ inline specials print nothing; 0x0A wraps */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0xFF03); ws_push(s, 0xFF21); ws_push(s, 0xFF11); ws_push(s, 0x3042);
    ws_push(s, 0x0A); ws_push(s, 0x3042);
    layout_ws(m, s);
    CHECK(m->ng == 2 && m->g[0].x == 0 && m->g[1].y == 28);
}

/* A full glyph store drops the excess but keeps laying out (the pen still moves). */
static void test_store_bounds(WStr *s)
{
    MGlyph small[8];
    Msg m;
    memset(&m, 0, sizeof m);
    msg_bind(&m, small, (int)COUNT_OF(small));
    msg_config(&m, &JA_CFG);
    msg_reset(&m);
    ws_set(s, NULL, 0); rep(s, 0x3042, 20);
    CHECK(layout_ws(&m, s) == 20 && m.ng == 8 && m.x == 20 * 23);   /* one line, 23 px pitch */
}

static void test_names(void)
{
    ws_set_utf8(&g_name_a, "\xe4\xba\xba\xe8\xa6\x8b");   /* 人見 */
    ws_set_utf8(&g_name_b, "\xe5\xba\x83\xe4\xbb\x8b");   /* 広介 */
    ws_set_utf8(&g_name_xy, "XY");
    ws_set_utf8(&g_name_loc, "L");
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa1\xef\xbc\x8a\xef\xbc\xa2",                 /* ＊Ａ＊Ｂ */
                    "\xe4\xba\xba\xe8\xa6\x8b\xe5\xba\x83\xe4\xbb\x8b"));
    CHECK(expand_eq("\xe3\x80\x8c\xef\xbc\x8a\xef\xbc\xa2\xe3\x80\x8d",                 /* 「＊Ｂ」 */
                    "\xe3\x80\x8c\xe5\xba\x83\xe4\xbb\x8b\xe3\x80\x8d"));
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa2\xef\xbc\x90", "\xe5\xba\x83"));              /* ＊Ｂ０ → 広 */
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa2\xef\xbc\x91", "\xe4\xbb\x8b"));              /* ＊Ｂ１ → 介 */
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa2\xef\xbc\x95", ""));                         /* out of range */
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa2\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99"   /* ＊Ｂ + 11 x ９ */
                    "\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99\xef\xbc\x99",
                    ""));                                                                  /* no int overflow */
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xa1\xef\xbc\xa2", "XY"));                       /* ＊ＡＢ = idx 27 */
    CHECK(expand_eq("\xef\xbc\x85\xef\xbc\xa1", "L"));                                    /* ％Ａ local */
    CHECK(expand_eq("\xef\xbc\x8a" "a", "\xef\xbc\x8a" "a"));                              /* not a token */
    CHECK(expand_eq("\xef\xbc\x8a\xef\xbc\xba", "\xef\xbc\x8a\xef\xbc\xba"));              /* unresolved: kept */
    ws_free(&g_name_a); ws_free(&g_name_b); ws_free(&g_name_xy); ws_free(&g_name_loc);
}

/* ---- typing and replay (TXT §5.4, ENGINE_SPEC §2.11) ---------------------------------------------------------- */
static int frames_until_typed(Mwnd *w, bool run_open_anim)
{
    int frames = 0;
    while (w->printing && frames < 1000) {
        w->clock += 16;
        if (run_open_anim && w->open_anim >= 0 && (w->open_anim += 16) >= w->cfg.open_time) w->open_anim = -1;
        mwnd_display_update(w);
        frames++;
    }
    return frames;
}

static void start_print(Mwnd *w, const WStr *s)
{
    msg_reset(&w->msg);
    layout_ws(&w->msg, s);
    w->printing = true;
    mwnd_display_update(w);
}

/* N glyphs at 20 ms finish after ceil(20N/16) frames of 16 ms; a closed window runs its open animation first. */
static void test_typing(Mwnd *w, WStr *s)
{
    ws_set(s, NULL, 0); rep(s, 0x3042, 37);
    w->open = true;
    start_print(w, s);
    CHECK(frames_until_typed(w, false) == (37 * 20 + 15) / 16);
    w->open = false; w->printing = false;
    w->clock = 0;
    start_print(w, s);
    CHECK(w->open && w->open_anim == 0);
    int want = (250 + 15) / 16 + (37 * 20 + 15) / 16, frames = frames_until_typed(w, true);
    CHECK(frames >= want - 1 && frames <= want + 1);
    /* click: finish at once */
    start_print(w, s);
    CHECK(mwnd_has_unrevealed(w));
    mwnd_finish_printing(w);
    CHECK(!w->printing && w->msg.shown == w->msg.ng);
}

static void test_replay(Mwnd *w, const WStr *s)
{
    msg_reset(&w->msg);
    mwnd_rp_clear(w);
    mwnd_rp_add(w, RP_SIZE, 24, NULL, 0);
    mwnd_rp_add(w, RP_TEXT, 0, s->p, s->len);
    mwnd_rp_add(w, RP_NL, 0, NULL, 0);
    mwnd_rp_add(w, RP_SIZE, 18, NULL, 0);
    mwnd_rp_add(w, RP_TEXT, 0, s->p, 5);
    mwnd_replay(w);
    Msg ref;
    msg_fresh(&ref, &JA_CFG, g_ref_store);
    layout_ws(&ref, s); msg_newline(&ref); msg_set_size(&ref, 18); msg_layout(&ref, s->p, 5);
    CHECK(w->msg.ng == ref.ng && w->msg.shown == w->msg.ng);
    CHECK(ref.ng > 0 && w->msg.g[ref.ng - 1].x == ref.g[ref.ng - 1].x &&
          w->msg.g[ref.ng - 1].y == ref.g[ref.ng - 1].y);
    mwnd_rp_clear(w);
}

static void test_window(WStr *s)
{
    mwnd_window_init(&g_tw, MWND_CNT, &JA_CFG);
    int saved_speed = sys_state()->msg_speed;
    bool nowait = false, no_anime = false;
    sys_state()->msg_speed = 20;
    mwnd_test_swap_timing(&nowait, &no_anime);
    test_typing(&g_tw, s);
    test_replay(&g_tw, s);
    mwnd_test_swap_timing(&nowait, &no_anime);
    sys_state()->msg_speed = saved_speed;
    mwnd_window_free(&g_tw);
}

/* ---- backlog: koe and name attach to the next text only ------------------------------------------------------ */
/* The built-in fallback face (vendored Kosugi) covers kana, kanji, full-width forms and ASCII. */
static void test_builtin_font(void)
{
    CHECK(font_builtin_covers("\xe3\x81\x95\xe3\x82\x88\xe3\x81\xaa\xe3\x82\x89"     /* さよなら */
                              "\xe6\x95\x99\xe3\x81\x88\xe3\x81\xa6"                     /* 教えて */
                              "\xe3\x80\x8c\xe3\x80\x8d\xe2\x80\xa5\xef\xbc\x8a"     /* 「」‥＊ */
                              "Sayonara wo Oshiete 0123456789"));
}

static void test_backlog(void)
{
    WStr t = {0}, n = {0};
    ws_set_utf8(&t, "text"); ws_set_utf8(&n, "name");
    int before = backlog_count();
    backlog_add_koe(5600037, -1, 1, 2);
    backlog_add_name(&n, 1, 2);
    backlog_add_text(&t, 1, 3);
    const BacklogEntry *e = backlog_get(backlog_count() - 1);
    CHECK(backlog_count() == (before < BACKLOG_MAX ? before + 1 : BACKLOG_MAX));
    CHECK(e && e->koe_no == 5600037 && ws_cmp(&e->name, &n) == 0 && ws_cmp(&e->text, &t) == 0 && e->line == 3);
    backlog_add_text(&t, 1, 4);
    e = backlog_get(backlog_count() - 1);
    CHECK(e && e->koe_no == -1 && e->name.len == 0);
    CHECK(backlog_get(-1) == NULL && backlog_get(backlog_count()) == NULL);
    ws_free(&t); ws_free(&n);
}

/* ---- font (AST §5.2): the 65-level ramp tops out at 251; effect n grows the bitmap by n ----------------------- */
static int max_alpha(const Glyph *g)
{
    int mx = 0;
    for (int i = 0; i < g->w * g->h; i++) if (g->a[i] > mx) mx = g->a[i];
    return mx;
}

/* More distinct glyphs than the cache holds: it flushes and keeps answering correctly. */
static void test_glyph_cache_bound(const Glyph *ref)
{
    for (uint16_t c = 0x4E00; c < 0x4E00 + 5000; c++) (void)font_glyph(c, 24, 0);
    const Glyph *g = font_glyph(0x3042, 24, 0);
    CHECK(g && g->w == ref->w && g->h == ref->h && g->ox == ref->ox && g->oy == ref->oy && g->adv == ref->adv);
}

static void test_font(void)
{
    const Glyph *g = font_glyph(0x3042, 24, 0);       /* each pointer is used before the next font call */
    if (!g || !g->a) { log_msg(LOG_WARN, "text_selftest: no font loaded, glyph checks skipped"); return; }
    Glyph g0 = *g;
    CHECK(max_alpha(g) == 251);
    g = font_glyph(0x3042, 24, 2);
    CHECK(g && g->w == g0.w + 2 && g->h == g0.h + 2);
    CHECK(g0.oy >= 0 && g0.oy + g0.h <= 26 && g0.w <= 24);
    g = font_glyph('A', 24, 0);
    CHECK(g0.adv == 24 && g && g->adv == 12);
    CHECK(font_glyph(0x3042, 0, 0) == NULL && font_glyph(0x3042, 256, 0) == NULL);
    test_glyph_cache_bound(&g0);
}

static uint32_t be32(const uint8_t *p)
{ return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

/* Offset of the table `tag` in a plain sfnt file (0 = missing; the test font is not a collection). */
static uint32_t sfnt_table_at(const uint8_t *d, size_t n, const char *tag)
{
    uint32_t count = n >= 12 ? (uint32_t)d[4] << 8 | d[5] : 0;
    for (uint32_t i = 0; i < count && 12 + 16 * (size_t)(i + 1) <= n; i++) {
        const uint8_t *rec = d + 12 + 16 * (size_t)i;
        if (!memcmp(rec, tag, 4)) return be32(rec + 8) < n ? be32(rec + 8) : 0;
    }
    return 0;
}

/* Glyph index and metrics that stb_truetype trusts: a loca entry past glyf or unitsPerEm 0 rejects the file
 * (review finding: a crafted loca made stb read outside the file). */
static void test_font_tables(uint8_t *d, size_t n)
{
    uint32_t head = sfnt_table_at(d, n, "head"), loca = sfnt_table_at(d, n, "loca");
    CHECK(head && loca && head + 54 <= n && loca + 8 <= n);
    if (!head || !loca || head + 54 > n || loca + 8 > n) return;
    uint8_t keep[4];
    memcpy(keep, d + loca + 4, 4);
    memset(d + loca + 4, 0x7f, 4);                    /* entry 1 (or 2) far beyond the glyf table */
    CHECK(!font_file_valid(d, n));
    memcpy(d + loca + 4, keep, 4);
    memcpy(keep, d + head + 18, 2);
    memset(d + head + 18, 0, 2);                      /* unitsPerEm 0: stb divides by it */
    CHECK(!font_file_valid(d, n));
    memcpy(d + head + 18, keep, 2);
    CHECK(font_file_valid(d, n));
}

/* Font files: the EN face's file passes; truncated or foreign data never reaches stb_truetype. */
static void test_font_files(void)
{
    static const char EN_TTF[] = "/en/font/sayooshieng-Regular.ttf";
    static const uint8_t junk[64] = { 't', 't', 'c', 'f', 0, 1, 0, 0, 0, 0, 0, 1, 0xff, 0xff, 0xff, 0xff };
    CHECK(!font_file_valid(NULL, 0) && !font_file_valid(junk, sizeof junk) && !font_file_valid(junk, 11));
    size_t n = 0;
    uint8_t *d = fs_exists(EN_TTF) ? fs_read_all(EN_TTF, &n) : NULL;
    if (!d) return;
    CHECK(font_file_valid(d, n));
    CHECK(!font_file_valid(d, 12) && !font_file_valid(d, 200) && !font_file_valid(d, n / 2));
    test_font_tables(d, n);
    fs_free(d);
}

/* ---- English layout (SPEC_ENPATCH §1.3.2–§1.3.4): 27 columns, word wrap, no hanging punctuation, long words ---- */
static void test_en_wrap(Msg *m, WStr *s)
{
    CHECK(m->limit_x == 622);                         /* 27*24 - 26 */
    /* ASCII pitch 11 px: 56 chars end at 616 <= 622; a word that would cross the limit moves down whole */
    ws_set_utf8(s, "aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa aaaaaaaaaa abcdefgh tail");   /* 54 + " abcdefgh" */
    layout_ws(m, s);
    int b = -1;
    for (int k = 0; k < m->ng; k++) if (m->g[k].code == 'b') b = k;
    CHECK(b > 1 && m->g[b - 1].code == 'a' && m->g[b - 1].x == 0 && m->g[b - 1].y == 28);   /* "abcdefgh" wrapped */
    CHECK(b > 1 && m->g[b - 2].code == ' ' && m->g[b - 2].y == 0);                       /* space ends line 1 */
    for (int k = 0; k < m->ng; k++) CHECK(m->g[k].x <= 622 - 11);                        /* nothing past the limit */
    /* "word." (5 cells from x = 572) would end at 627 > 622: the whole run wraps, punctuation included */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 'x', 51); ws_push(s, ' '); put_ascii(s, "word.");   /* 52 + 5 = 57 */
    layout_ws(m, s);
    CHECK(m->ng == 57 && m->g[52].code == 'w' && m->g[52].x == 0 && m->g[52].y == 28 && m->g[56].code == '.');
    /* no hanging punctuation: 。 after 27 full-width chars (x = 621) wraps instead of hanging (JA: burasage) */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 0x3042, 27); ws_push(s, 0x3002);
    layout_ws(m, s);
    CHECK(m->ng == 28 && m->g[26].y == 0 && m->g[27].code == 0x3002 && m->g[27].x == 0 && m->g[27].y == 28);
    /* a run longer than a line breaks at the limit */
    msg_reset(m);
    ws_set(s, NULL, 0); rep(s, 'z', 70);
    layout_ws(m, s);
    CHECK(m->g[55].y == 0 && m->g[55].x == 605 && m->g[56].y == 28 && m->g[56].x == 0);
    /* hanging indent is untouched (EN dialogue uses ASCII quotes, so it never fires there) */
    msg_reset(m);
    ws_set(s, NULL, 0); ws_push(s, 0x300C); rep(s, 0x3042, 40);
    layout_ws(m, s);
    CHECK(m->g[27].x == 23 && m->g[27].y == 28);
}

/* Comma kerning with the EN face: "F," -> -8 px at size 24, "r," -> -6; nothing after a digit. */
static void test_en_kerning(Msg *m, WStr *s)
{
    if (!font_has_face(FONT_EN)) {
        log_msg(LOG_WARN, "text_selftest: no EN face (sayooshieng-Regular.ttf); kerning checks skipped");
        return;
    }
    font_select_for_language(LANG_EN);
    CHECK(font_kern_px('F', ',', 24) == -8 && font_kern_px('r', ',', 24) == -6 && font_kern_px('A', ',', 24) == 0);
    CHECK(font_kern_px('I', ',', 24) == -4 && font_kern_px('y', ',', 24) == -3 && font_kern_px('1', ',', 24) == 0);
    msg_reset(m);
    ws_set_utf8(s, "Fr, F, 1, x");
    layout_ws(m, s);
    CHECK(m->ng == 11 && m->g[2].x == 22 - 6 && m->g[3].x == 33 - 6);       /* "r," moved, and so did the pen */
    CHECK(m->g[5].x == 49 - 8 && m->g[7].x == 63 && m->g[8].x == 74);       /* "F," kerned, "1," not */
    font_select_for_language(LANG_JA);
}

/* Choices: a long item wraps under its start and all hit rects get the widest width. */
static void test_en_choices(const MwndCfg *cfg)
{
    Mwnd *w = &g_tw;
    mwnd_window_init(w, MWND_CNT, cfg);
    w->sel_x = 0; w->sel_y = 0;
    WStr a = {0}, l = {0};
    ws_set_utf8(&a, "[ Short. ]");
    ws_set_utf8(&l, "[ This is a rather long choice that certainly does not fit on one line of the window. ]");
    sel_add_item(w, &a, 1, 0);
    sel_add_item(w, &l, 1, 0);
    CHECK(w->nsel == 2 && w->sel[0].r.y1 - w->sel[0].r.y0 == 28 && w->sel[1].r.y1 - w->sel[1].r.y0 == 56);
    CHECK(w->sel[0].r.x1 == w->sel[1].r.x1 && w->sel[1].r.y0 == w->sel[0].r.y1);
    for (int k = w->sel[1].g0; k < w->sel[1].g1; k++) CHECK(w->sel_g[k].x <= 622 - 11);
    for (int k = 0; k < SEL_ITEM_MAX + 4; k++) sel_add_item(w, &a, 1, 0);   /* a full list drops the excess */
    CHECK(w->nsel == SEL_ITEM_MAX && w->nsel_g <= SEL_GLYPH_MAX);
    sel_reset(w);
    mwnd_window_free(w);
    ws_free(&a); ws_free(&l);
}

static void test_english(WStr *s)
{
    MwndCfg cfg = JA_CFG;
    cfg.cols = 27;
    Msg m;
    lang_test_override(LANG_EN);
    msg_fresh(&m, &cfg, g_store);
    test_en_wrap(&m, s);
    test_en_kerning(&m, s);
    test_en_choices(&cfg);
    lang_test_override(LANG_JA);
}

int txt_selftest(void)
{
    g_fails = 0;
    if (!font_has_face(FONT_JA) && !font_has_face(FONT_EN)) (void)font_init(NULL);   /* no font: glyph checks skip */
    lang_test_override(LANG_JA);                     /* the Japanese rules, whatever data language is active */
    Msg m;
    msg_fresh(&m, &JA_CFG, g_store);
    WStr s = {0};
    test_widths(&m);
    test_wrap(&m, &s);
    test_indent(&m, &s);
    test_overflow_size(&m, &s);
    test_store_bounds(&s);
    test_names();
    test_window(&s);
    test_backlog();
    test_builtin_font();
    test_font();
    test_font_files();
    test_english(&s);
    lang_test_override(LANG_NONE);
    ws_free(&s);
    log_msg(g_fails ? LOG_ERR : LOG_INFO, "text_selftest: %s (%d failures)", g_fails ? "FAIL" : "ok", g_fails);
    return g_fails;
}
