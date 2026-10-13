/* txt/msglayout.c — message object layout: wrap, kinsoku (burasage), talk indent, overflow, name expansion
 * (TXT §5.2–§5.3, §3.2 step 2; exe FUN_005e7690 / FUN_005e7fe0 / FUN_005e72c0 / FUN_00599430 / FUN_00597fc0).
 *
 * A Msg lays glyphs into storage its owner binds (msg_bind): a window's page, a scratch buffer for a choice, a
 * test's stack array. A full store drops further glyphs (logged once) while the pen keeps moving, so the layout of
 * everything that fits is unchanged. Every loop consumes at least one input unit per iteration. */
#include "txt/txt_internal.h"
#include "core/log.h"
#include "core/str.h"
#include "txt/font.h"
#include "txt/lang.h"

enum {
    CH_SPACE = 0x20, CH_IDEO_SPACE = 0x3000, CH_LF = 0x0A, CH_BEL = 0x07,
    CH_KAGI_OPEN = 0x300C, CH_KAGI_CLOSE = 0x300D,             /* 「 」 */
    CH_NIJU_OPEN = 0x300E, CH_NIJU_CLOSE = 0x300F,             /* 『 』 */
    CH_PAREN_OPEN = 0xFF08, CH_PAREN_CLOSE = 0xFF09,           /* （ ） */
    CH_FW_HASH = 0xFF03, CH_FW_A = 0xFF21, CH_FW_B = 0xFF22, CH_FW_Z = 0xFF3A,
    CH_FW_0 = 0xFF10, CH_FW_9 = 0xFF19,
    CH_FW_STAR = 0xFF0A, CH_FW_PERCENT = 0xFF05,               /* ＊ global name, ％ local name */
    NAME_LETTERS = 26,
};

void txt_cap_overflow(bool *logged, const char *what, int cap)
{
    log_once(logged, LOG_WARN, "txt: %s is full (%d); the excess is dropped", what, cap);
}

/* §1.6 [I]: half-width = any code unit CP932 encodes in one byte. */
int msg_char_width(uint16_t code, int size)
{
    bool half = (code >= 0x20 && code <= 0x7E) || (code >= 0xFF61 && code <= 0xFF9F);
    return half ? size / 2 : size;
}

/* FUN_005e72c0: characters that may not start a line (they hang into the margin instead). */
bool msg_no_line_start(uint16_t c)
{
    static const uint16_t tbl[] = {
        0x3002, 0x3001, 0xFF01, 0xFF1F, 0xFF1A, 0xFF1B, 0x300D, 0x300F, 0xFF09, 0x3011, 0xFF5D, 0xFF3D,
        0x3009, 0x300B, 0x201D, 0x2019, 0x301F, 0xFF5E, 0x30FC, 0x2026, 0x2025, 0x30FB, 0xFF0E, 0xFF0C,
        0x309B, 0x309C, 0x3041, 0x3043, 0x3045, 0x3047, 0x3049, 0x3063, 0x3083, 0x3085, 0x3087, 0x308E,
        0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9, 0x30C3, 0x30E3, 0x30E5, 0x30E7, 0x30EE, 0x30F5, 0x30F6,
        0xFF9E, 0xFF9F, 0xFF61, 0xFF64, 0x21, 0x3F, 0x3A, 0x3B, 0xFF63, 0x29, 0x5D, 0x3E, 0x7D, 0x27, 0x22,
        0xFF70, 0xFF65, 0x2E, 0x2C, 0xFF67, 0xFF68, 0xFF69, 0xFF6A, 0xFF6B, 0xFF6F, 0xFF6C, 0xFF6D, 0xFF6E,
    };
    for (size_t i = 0; i < COUNT_OF(tbl); i++)
        if (tbl[i] == c) return true;
    return false;
}

void msg_bind(Msg *m, MGlyph *storage, int cap)
{
    ASSERT(cap >= 0 && (storage != NULL || cap == 0));
    m->g = storage;
    m->cap = cap;
    m->ng = m->shown = 0;
}

/* Sizes and limits from the window config; the current character size (§1.4.2) survives page clears
 * (seen603: `size(18)` is set before the 84 that runs the deferred clear). */
void msg_config(Msg *m, const MwndCfg *cfg)
{
    m->default_size = cfg->size;
    m->size = cfg->size;
    m->space_x = cfg->space_x;
    m->space_y = cfg->space_y;
    /* exe 0x5e66e0 (EN 0x5dc134): cols*size + (cols-1)*space (SPEC_ENPATCH §1.3.2): MWND.002 JA 599, EN 622, 444 */
    m->limit_x = cfg->cols * cfg->size + (cfg->cols - 1) * cfg->space_x;
    m->limit_y = cfg->rows * cfg->size + (cfg->rows - 1) * cfg->space_y;
}

void msg_reset(Msg *m)
{
    m->ng = m->shown = 0;
    m->x = m->y = 0;
    m->indent_x = m->depth = 0;
    m->open_ch = 0;
    m->at_line_start = true;
    m->moji = m->fuchi = m->shadow = -1;
    m->started = false;
}

void msg_set_size(Msg *m, int size)
{
    m->size = size < 0 ? m->default_size : size > MSG_SIZE_MAX ? MSG_SIZE_MAX : size;
}

/* FUN_005e7fe0 (horizontal): back to the indent column, one line down. */
static void msg_wrap(Msg *m)
{
    m->x = m->indent_x;
    m->y += m->size + m->space_y;
}

/* FUN_005e3070: explicit newline resets the talk indent. */
void msg_newline(Msg *m)
{
    m->indent_x = 0; m->open_ch = 0; m->depth = 0;
    msg_wrap(m);
    m->at_line_start = true;
}

/* FUN_005e2ee0 [I]: are `lines` more lines still inside the row limit? */
bool msg_fits_lines(const Msg *m, int lines)
{
    return m->y + lines * (m->size + m->space_y) < m->limit_y;
}

static uint8_t colour_of(int over, int def) { return (uint8_t)(over >= 0 ? over : def); }

static void push_glyph(Msg *m, uint16_t code, int x, int y)
{
    static bool logged;
    if (m->ng >= m->cap) { txt_cap_overflow(&logged, "message glyph store", m->cap); return; }
    MGlyph *g = &m->g[m->ng++];
    g->code = code; g->x = (int16_t)x; g->y = (int16_t)y; g->size = (uint8_t)m->size;
    g->moji = colour_of(m->moji, m->def_moji);
    g->fuchi = colour_of(m->fuchi, m->def_fuchi);
    g->shadow = colour_of(m->shadow, m->def_shadow);
}

static bool is_fw_digit(uint16_t c) { return c >= CH_FW_0 && c <= CH_FW_9; }
static bool is_fw_letter(uint16_t c) { return c >= CH_FW_A && c <= CH_FW_Z; }

/* FUN_00599430: `＃Ａ`/`＃Ｂ` + full-width digits is an inline special (unused by the game: printed as nothing).
 * Returns its length in units, 0 if s[i] does not start one. */
static int inline_special_len(const uint16_t *s, int n, int i)
{
    if (s[i] != CH_FW_HASH || i + 2 >= n || (s[i + 1] != CH_FW_A && s[i + 1] != CH_FW_B) || !is_fw_digit(s[i + 2]))
        return 0;
    int j = i + 2;
    while (j < n && is_fw_digit(s[j])) j++;
    return j - i;
}

static bool is_word_char(uint16_t c) { return c >= 0x21 && c <= 0x7E; }
static bool is_letter(uint16_t c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

/* English line breaking of the patched exe (SPEC_ENPATCH §1.3.2, hook H4 at EN 0x5ddd36): a printable ASCII char
 * that starts a word (previous code unit not in 0x21..0x7e) wraps when the whole run, measured with the current
 * char's advance, would end past the limit, provided the run fits on a line at all. Everything else wraps as soon
 * as it ends past the limit: there is no hard limit and no kinsoku hanging (burasage) in English. */
static bool latin_wrap(const Msg *m, const uint16_t *s, int n, int i, int w)
{
    int adv = w + m->space_x;
    uint16_t c = s[i];
    if (is_word_char(c) && (i == 0 || !is_word_char(s[i - 1])) && adv > 0) {
        int k = i;
        while (k < n && is_word_char(s[k])) k++;
        long word_w = (long)(k - i) * adv;
        if (word_w <= m->limit_x - m->indent_x) return m->x + word_w > m->limit_x;
    }
    return m->x + adv > m->limit_x;
}

/* Japanese: a hard limit with one character of hang room, and a soft limit only kinsoku characters may pass. */
static bool kinsoku_wrap(const Msg *m, uint16_t c, int w)
{
    int x_end = m->x + m->space_x + w;
    return x_end > m->default_size + m->limit_x || (x_end > m->limit_x && !msg_no_line_start(c));
}

/* Comma kerning (H5 at EN 0x5ddf28, SPEC_ENPATCH §1.3.3): a ',' in the half cell right after a letter of the same
 * size on the same line moves left by the face's kern pair (letter, comma); so does the pen. */
static void kern_comma(Msg *m, uint16_t c)
{
    if (c != ',' || m->ng <= 0 || m->size < 1) return;
    const MGlyph *pg = &m->g[m->ng - 1];
    if (is_letter(pg->code) && pg->y == m->y && pg->size == m->size && pg->x + m->size / 2 + m->space_x == m->x)
        m->x += font_kern_px(pg->code, c, m->size);
}

static bool closes(uint16_t open, uint16_t c)
{
    return (open == CH_KAGI_OPEN && c == CH_KAGI_CLOSE) || (open == CH_NIJU_OPEN && c == CH_NIJU_CLOSE) ||
           (open == CH_PAREN_OPEN && c == CH_PAREN_CLOSE);
}

/* Talk indent (FUN_005e81d0): 「『（ at the line start sets the hanging indent after the bracket; the matching
 * closer at the same depth ends it. */
static void talk_indent(Msg *m, uint16_t c)
{
    if (c == CH_KAGI_OPEN || c == CH_NIJU_OPEN || c == CH_PAREN_OPEN) {
        if (m->at_line_start) { m->indent_x = m->x; m->depth = 1; m->open_ch = c; }
        else if (c == m->open_ch) m->depth++;
    }
    if (m->depth > 0) {
        if (closes(m->open_ch, c)) m->depth--;
        if (m->depth == 0) { m->indent_x = 0; m->open_ch = 0; }
    }
    m->at_line_start = false;
}

int msg_layout(Msg *m, const uint16_t *s, int n)
{
    bool latin = lang_latin_layout(), kern = latin && font_kerning_active();
    int i = 0;
    while (i < n) {
        int special = inline_special_len(s, n, i);
        uint16_t c = s[i];
        if (special > 0) { i += special; continue; }
        if (c == CH_LF) { msg_wrap(m); i++; continue; }
        if (c == CH_BEL) { msg_newline(m); i++; continue; }
        int w = msg_char_width(c, m->size);
        if (latin ? latin_wrap(m, s, n, i, w) : kinsoku_wrap(m, c, w)) {
            msg_wrap(m);
            if (c == CH_SPACE || c == CH_IDEO_SPACE) { i++; continue; }   /* a wrapped space is dropped */
        }
        if (m->y >= m->limit_y) return i;                                /* overflow: the rest is the remainder */
        if (kern && m->size <= 512) kern_comma(m, c);
        push_glyph(m, c, m->x, m->y);
        m->x += m->space_x + w;
        talk_indent(m, c);
        i++;
    }
    return n;
}

/* One name token at in[i] (＊ or ％ followed by Ａ–Ｚ): bank index from one or two letters (SYS §8), optional
 * full-width digit index. Returns the index just past the token. */
static int name_token(const WStr *in, int i, int *idx, int *digit)
{
    int j = i + 1;
    *idx = in->p[j++] - CH_FW_A;
    if (j < in->len && is_fw_letter(in->p[j])) *idx = (*idx + 1) * NAME_LETTERS + (in->p[j++] - CH_FW_A);
    *digit = -1;
    for (; j < in->len && is_fw_digit(in->p[j]); j++) {
        if (*digit < WSTR_MAX_LEN)          /* beyond every WStr length already: stop growing (no int overflow) */
            *digit = (*digit < 0 ? 0 : *digit * 10) + (in->p[j] - CH_FW_0);
    }
    return j;
}

/* FUN_00597fc0: ＊ (global, root 107) / ％ (local, root 106) + one or two of Ａ–Ｚ, optional full-width digit index. */
static WStr g_expand;   /* msg_expand_names' output scratch: kept between calls (in may alias out) */

void msg_shutdown(void) { ws_free(&g_expand); }

void msg_expand_names(const WStr *in, WStr *out, NameResolver res)
{
    WStr *o = &g_expand;
    ws_set(o, NULL, 0);
    for (int i = 0; i < in->len; i++) {
        uint16_t c = in->p[i];
        bool token = (c == CH_FW_STAR || c == CH_FW_PERCENT) && i + 1 < in->len && is_fw_letter(in->p[i + 1]);
        if (!token) { ws_push(o, c); continue; }
        int idx, digit, j = name_token(in, i, &idx, &digit);
        const WStr *v = res ? res(c == CH_FW_STAR, idx) : NULL;
        if (!v) {                                                    /* unresolved: keep the token as written */
            for (int k = i; k < j; k++) ws_push(o, in->p[k]);
        } else if (digit < 0) {
            for (int k = 0; k < v->len; k++) ws_push(o, v->p[k]);
        } else if (digit < v->len) {                                 /* FUN_00581010(s, n, 1): one character */
            ws_push(o, v->p[digit]);
        }
        i = j - 1;
    }
    ws_copy(out, o);
}
