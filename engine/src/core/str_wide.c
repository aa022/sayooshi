/* core/str_wide.c — WStr (heap UTF-16 script strings, ENGINE_SPEC §2.3) and WBuf (bounded UTF-16 builder).
 * WStr buffers live on the tracked heap (MEM_STR). Lengths are int (the VM's unit) and capped at WSTR_MAX_LEN:
 * a script that builds a longer string gets it truncated (logged once) instead of exhausting memory. */
#include "core/str.h"
#include "core/log.h"
#include "core/mem.h"
#include <string.h>

static void too_long(void)
{
    static bool logged;
    if (!logged) log_msg(LOG_ERR, "str: a string exceeded %d UTF-16 units and was truncated", WSTR_MAX_LEN);
    logged = true;
}

static bool ws_grow(WStr *s, int cap)
{
    uint16_t *p = s->p ? mem_try_realloc_array(s->p, (size_t)cap, sizeof *s->p)
                       : mem_try_alloc_array((size_t)cap, sizeof *s->p, MEM_STR);
    if (!p) return false;
    s->p = p;
    s->cap = cap;
    return true;
}

/* Room for n units plus the terminator; returns n clamped to WSTR_MAX_LEN and to what the MEM_STR heap budget
 * still allows (core/mem.h): a string the budget refuses is truncated like an over-long one (logged once). */
static int ws_reserve(WStr *s, int n)
{
    static bool budget_logged;
    if (n < 0) n = 0;
    if (n > WSTR_MAX_LEN) { too_long(); n = WSTR_MAX_LEN; }
    if (n + 1 <= s->cap) return n;
    int cap = s->cap ? s->cap : 16;
    while (cap < n + 1) cap = cap > WSTR_MAX_LEN / 2 ? WSTR_MAX_LEN + 1 : cap * 2;
    if (ws_grow(s, cap) || ws_grow(s, n + 1)) return n;
    log_once(&budget_logged, LOG_ERR, "str: the string heap budget is exhausted; strings are truncated");
    if (!s->p) {                               /* a minimal buffer always fits in the budget's headroom */
        s->p = mem_alloc_array(16, sizeof *s->p, MEM_STR);
        s->cap = 16;
    }
    return n < s->cap ? n : s->cap - 1;
}

void ws_set(WStr *s, const uint16_t *u, int n)
{
    if (!u) n = 0;
    else if (n < 0) { n = 0; while (n < WSTR_MAX_LEN && u[n]) n++; }
    n = ws_reserve(s, n);
    if (n && u) memmove(s->p, u, (size_t)n * sizeof *u);   /* u may point into s->p (n <= len: no reallocation) */
    s->len = n;
    s->p[n] = 0;
}

void ws_push(WStr *s, uint16_t c)
{
    if (ws_reserve(s, s->len + 1) <= s->len) return;   /* at the cap */
    s->p[s->len++] = c;
    s->p[s->len] = 0;
}

/* Lenient legacy decoder, kept bit-exact for trace compatibility: no overlong/surrogate checks; a stray
 * continuation byte or a truncated sequence becomes U+FFFD; values above U+10FFFF become U+FFFD. */
void ws_set_utf8(WStr *s, const char *utf8)
{
    const unsigned char *q = (const unsigned char *)(utf8 ? utf8 : "");
    s->len = 0;
    (void)ws_reserve(s, 0);
    s->p[0] = 0;
    while (*q) {
        uint32_t c = *q++;
        int more = 0;
        if (c >= 0xF0) { c &= 0x07; more = 3; }
        else if (c >= 0xE0) { c &= 0x0F; more = 2; }
        else if (c >= 0xC0) { c &= 0x1F; more = 1; }
        else if (c >= 0x80) c = 0xFFFD;
        for (; more && (*q & 0xC0) == 0x80; more--) c = (c << 6) | (*q++ & 0x3F);
        if (more) c = 0xFFFD;
        if (c >= 0x10000 && c <= 0x10FFFF) {
            c -= 0x10000;
            ws_push(s, (uint16_t)(0xD800 + (c >> 10)));
            ws_push(s, (uint16_t)(0xDC00 + (c & 0x3FF)));
        } else {
            ws_push(s, c > 0xFFFF ? 0xFFFD : (uint16_t)c);
        }
    }
}

bool ws_from_utf8(WStr *s, Str utf8)
{
    bool ok = true;
    size_t pos = 0;
    uint32_t cp;
    s->len = 0;
    (void)ws_reserve(s, 0);
    s->p[0] = 0;
    while (pos < utf8.n) {
        ok = utf_decode8(utf8, &pos, &cp) && ok;
        uint16_t u[2];
        size_t k = utf_encode16(cp, u);
        for (size_t i = 0; i < k; i++) ws_push(s, u[i]);
    }
    return ok;
}

void ws_copy(WStr *dst, const WStr *src)
{
    if (dst == src) return;
    ws_set(dst, src->p, src->len);
}

void ws_cat(WStr *dst, const WStr *src)
{
    int n = src->len, old = dst->len;
    int total = ws_reserve(dst, old + n);
    n = total - old;
    if (n > 0) memmove(dst->p + old, src->p, (size_t)n * sizeof *src->p);   /* src may alias dst */
    dst->len = old + (n > 0 ? n : 0);
    dst->p[dst->len] = 0;
}

void ws_free(WStr *s)
{
    mem_free(s->p);
    s->p = NULL;
    s->len = s->cap = 0;
}

static int ws_cmp_impl(const WStr *a, const WStr *b, bool fold)
{
    int n = a->len < b->len ? a->len : b->len;
    for (int i = 0; i < n; i++) {
        unsigned x = a->p[i], y = b->p[i];
        if (fold) {
            if (x >= 'A' && x <= 'Z') x += 32;
            if (y >= 'A' && y <= 'Z') y += 32;
        }
        if (x != y) return x < y ? -1 : 1;
    }
    return a->len < b->len ? -1 : a->len > b->len ? 1 : 0;
}

int ws_cmp(const WStr *a, const WStr *b) { return ws_cmp_impl(a, b, false); }
int ws_cmp_fold(const WStr *a, const WStr *b) { return ws_cmp_impl(a, b, true); }

/* One unit (or pair) of s at *i as WTF-8: a valid pair -> 4 bytes, a lone surrogate -> its 3-byte form. */
static size_t wtf8_unit(const WStr *s, int *i, char out[4])
{
    uint32_t c = s->p[*i];
    if (c >= 0xD800 && c <= 0xDBFF && *i + 1 < s->len && s->p[*i + 1] >= 0xDC00 && s->p[*i + 1] <= 0xDFFF) {
        c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(s->p[++*i] - 0xDC00);
        return utf_encode8(c, out);
    }
    if (c < 0x80) { out[0] = (char)c; return 1; }
    if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6)); out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
    out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F));
    return 3;
}

/* A tracked heap block (MEM_STR), released with ws_utf8_free. Prefer ws_to_utf8 + StrBuf for temporaries. */
char *ws_utf8(const WStr *s)
{
    size_t cap = (size_t)(s->len > 0 ? s->len : 0) * 3 + 1;   /* <= 3 bytes per unit; a pair is 4 bytes for 2 units */
    char *out = mem_alloc(cap, MEM_STR);
    size_t o = 0;
    for (int i = 0; i < s->len; i++) {
        char u[4];
        size_t k = wtf8_unit(s, &i, u);
        memcpy(out + o, u, k);
        o += k;
    }
    out[o] = 0;
    return out;
}

void ws_utf8_free(char *u) { mem_free(u); }

const char *ws_utf8_arena(Arena *a, const WStr *s, size_t max_bytes)
{
    ASSERT(a && max_bytes >= 1);
    size_t cap = core_min_sz((size_t)(s && s->len > 0 ? s->len : 0) * 3 + 1, max_bytes);   /* WTF-8 bound */
    StrBuf b;
    sb_init(&b, arena_alloc_uninit(a, cap, 1), cap);
    if (s) (void)ws_to_utf8(s, &b);            /* cut (at a code point) only beyond max_bytes: the contract */
    return sb_cstr(&b);
}

bool ws_to_utf8(const WStr *s, StrBuf *out)
{
    bool ok = true;
    for (int i = 0; i < s->len; i++) {
        char u[4];
        size_t k = wtf8_unit(s, &i, u);
        ok = sb_put(out, str_make(u, k)) && ok;
    }
    return ok;
}

/* ---- WBuf ---- */

void wb_init(WBuf *b, uint16_t *storage, size_t cap)
{
    b->p = storage;
    b->cap = cap;
    b->n = 0;
    b->trunc = cap == 0;
    if (cap) b->p[0] = 0;
}

bool wb_put(WBuf *b, const uint16_t *u, size_t n)
{
    if (b->cap == 0) { b->trunc = true; return false; }
    size_t room = b->cap - 1 - b->n, k = n <= room ? n : room;
    if (k && k < n && u[k - 1] >= 0xD800 && u[k - 1] <= 0xDBFF) k--;   /* never split a surrogate pair */
    if (k) memcpy(b->p + b->n, u, k * sizeof *u);
    b->n += k;
    b->p[b->n] = 0;
    if (k < n) b->trunc = true;
    return k == n;
}

bool wb_put_cp(WBuf *b, uint32_t cp)
{
    uint16_t u[2];
    size_t k = utf_encode16(cp, u);
    return wb_put(b, u, k);
}

bool wb_put_utf8(WBuf *b, Str s)
{
    bool ok = true;
    size_t pos = 0;
    uint32_t cp;
    while (pos < s.n) {
        ok = utf_decode8(s, &pos, &cp) && ok;
        ok = wb_put_cp(b, cp) && ok;
    }
    return ok;
}
