/* core/str.c — Str slices, fixed-buffer copy/format with truncation reports, the StrBuf builder. */
#include "core/str.h"
#include "core/mem.h"
#include <SDL3/SDL.h>
#include <string.h>

static int fold(int c) { return (c >= 'A' && c <= 'Z') ? c + 32 : c; }

Str str_c(const char *s) { return str_make(s ? s : "", s ? strlen(s) : 0); }
Str str_make(const char *p, size_t n) { Str s = { p ? p : "", p ? n : 0 }; return s; }

bool str_eq(Str a, Str b) { return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0); }
bool str_eq_fold(Str a, Str b) { return a.n == b.n && str_cmp_fold(a, b) == 0; }

static int cmp_impl(Str a, Str b, bool f)
{
    size_t n = a.n < b.n ? a.n : b.n;
    for (size_t i = 0; i < n; i++) {
        int x = (unsigned char)a.p[i], y = (unsigned char)b.p[i];
        if (f) { x = fold(x); y = fold(y); }
        if (x != y) return x < y ? -1 : 1;
    }
    return a.n < b.n ? -1 : a.n > b.n ? 1 : 0;
}

int str_cmp(Str a, Str b) { return cmp_impl(a, b, false); }
int str_cmp_fold(Str a, Str b) { return cmp_impl(a, b, true); }

bool str_starts_with(Str s, Str prefix) { return s.n >= prefix.n && str_eq(str_sub(s, 0, prefix.n), prefix); }
bool str_ends_with_fold(Str s, Str suffix)
{
    return s.n >= suffix.n && str_eq_fold(str_sub(s, s.n - suffix.n, suffix.n), suffix);
}

Str str_sub(Str s, size_t pos, size_t len)
{
    if (pos > s.n) pos = s.n;
    if (len > s.n - pos) len = s.n - pos;
    return str_make(s.p + pos, len);
}

bool str_find_char(Str s, char c, size_t *pos)
{
    for (size_t i = 0; i < s.n; i++)
        if (s.p[i] == c) { *pos = i; return true; }
    return false;
}

bool str_rfind_char(Str s, char c, size_t *pos)
{
    for (size_t i = s.n; i > 0; i--)
        if (s.p[i - 1] == c) { *pos = i - 1; return true; }
    return false;
}

bool str_find(Str hay, Str needle, size_t *pos)
{
    if (needle.n == 0) { *pos = 0; return true; }
    for (size_t i = 0; i + needle.n <= hay.n; i++)
        if (memcmp(hay.p + i, needle.p, needle.n) == 0) { *pos = i; return true; }
    return false;
}

static bool is_space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

Str str_trim(Str s)
{
    size_t a = 0, b = s.n;
    while (a < b && is_space(s.p[a])) a++;
    while (b > a && is_space(s.p[b - 1])) b--;
    return str_sub(s, a, b - a);
}

bool str_split(Str *rest, char sep, Str *field)
{
    if (rest->n == 0 && rest->p == NULL) return false;
    size_t at;
    if (str_find_char(*rest, sep, &at)) {
        *field = str_sub(*rest, 0, at);
        *rest = str_sub(*rest, at + 1, rest->n);
    } else {
        *field = *rest;
        rest->p = NULL;                        /* exhausted: the next call returns false */
        rest->n = 0;
    }
    return true;
}

/* ---- arena strings ---- */

char *str_dup_arena(Arena *a, Str s)
{
    size_t n;
    ASSERT_MSG(core_add_sz(s.n, 1, &n), "str_dup_arena: length overflow");
    char *d = arena_alloc_uninit(a, n, 1);
    if (s.n) memcpy(d, s.p, s.n);
    d[s.n] = 0;
    return d;
}

char *str_cat_arena(Arena *a, const char *x, const char *y, const char *z)
{
    ASSERT(a && x && y);
    size_t nx = strlen(x), ny = strlen(y), nz = z ? strlen(z) : 0, n;
    bool fits = core_add_sz(nx, ny, &n) && core_add_sz(n, nz, &n) && core_add_sz(n, 1, &n);
    ASSERT_MSG(fits, "str_cat_arena: length overflow");
    char *s = arena_alloc_uninit(a, n, 1);
    memcpy(s, x, nx);
    memcpy(s + nx, y, ny);
    if (nz) memcpy(s + nx + ny, z, nz);
    s[n - 1] = 0;
    return s;
}

/* ---- fixed buffers ---- */

/* Largest k <= n such that cutting s at k does not split a UTF-8 sequence. */
static size_t utf8_cut(const char *s, size_t n, size_t k)
{
    if (k >= n) return n;
    while (k > 0 && ((unsigned char)s[k] & 0xC0) == 0x80) k--;
    return k;
}

/* Length of s[0..n) without a trailing incomplete UTF-8 sequence (what a byte-wise cut such as vsnprintf's leaves). */
static size_t utf8_complete(const char *s, size_t n)
{
    size_t i = n, cont = 0;
    while (i > 0 && cont < 3 && ((unsigned char)s[i - 1] & 0xC0) == 0x80) { i--; cont++; }
    if (i == 0) return n;
    unsigned char lead = (unsigned char)s[i - 1];
    size_t need = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return lead >= 0xC0 && need > cont + 1 ? i - 1 : n;
}

bool str_copy(char *dst, size_t cap, Str src)
{
    if (cap == 0) return false;
    size_t k = src.n < cap ? src.n : utf8_cut(src.p, src.n, cap - 1);
    if (k) memmove(dst, src.p, k);
    dst[k] = 0;
    return k == src.n;
}

bool str_copy_c(char *dst, size_t cap, const char *src) { return str_copy(dst, cap, str_c(src)); }

bool str_cat_c(char *dst, size_t cap, const char *src)
{
    if (cap == 0) return false;
    size_t have = 0;
    while (have < cap && dst[have]) have++;
    if (have >= cap) { dst[cap - 1] = 0; return false; }   /* dst was not terminated: repair, report */
    return str_copy(dst + have, cap - have, str_c(src));
}

bool str_vfmt(char *dst, size_t cap, const char *fmt, va_list ap)
{
    if (cap == 0) return false;
    int k = SDL_vsnprintf(dst, cap, fmt, ap);
    if (k < 0) { dst[0] = 0; return false; }
    if ((size_t)k >= cap) {
        dst[utf8_complete(dst, cap - 1)] = 0;
        return false;
    }
    return true;
}

void str_vfmt_cut(char *dst, size_t cap, const char *fmt, va_list ap)
{
    bool whole = str_vfmt(dst, cap, fmt, ap);
    (void)whole;                               /* the caller accepts a cut result (the contract) */
}

void str_fmt_cut(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    str_vfmt_cut(dst, cap, fmt, ap);
    va_end(ap);
}

bool str_fmt(char *dst, size_t cap, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = str_vfmt(dst, cap, fmt, ap);
    va_end(ap);
    return ok;
}

/* ---- StrBuf ---- */

void sb_init(StrBuf *b, char *storage, size_t cap)
{
    b->p = storage;
    b->cap = cap;
    sb_reset(b);
}

void sb_reset(StrBuf *b)
{
    b->n = 0;
    b->trunc = b->cap == 0;
    if (b->cap) b->p[0] = 0;
}

bool sb_put(StrBuf *b, Str s)
{
    if (b->cap == 0) { b->trunc = true; return false; }
    size_t room = b->cap - 1 - b->n;
    size_t k = s.n <= room ? s.n : utf8_cut(s.p, s.n, room);
    if (k) memcpy(b->p + b->n, s.p, k);
    b->n += k;
    b->p[b->n] = 0;
    if (k < s.n) b->trunc = true;
    return k == s.n;
}

bool sb_put_c(StrBuf *b, const char *s) { return sb_put(b, str_c(s)); }
bool sb_put_char(StrBuf *b, char c)
{
    if (b->cap == 0 || b->n + 1 >= b->cap) { b->trunc = true; return false; }
    b->p[b->n++] = c;
    b->p[b->n] = 0;
    return true;
}

bool sb_put_cp(StrBuf *b, uint32_t cp)
{
    char u[4];
    size_t k = utf_encode8(cp, u);
    return sb_put(b, str_make(u, k));
}

bool sb_vfmt(StrBuf *b, const char *fmt, va_list ap)
{
    if (b->cap == 0) { b->trunc = true; return false; }
    bool ok = str_vfmt(b->p + b->n, b->cap - b->n, fmt, ap);
    b->n += strlen(b->p + b->n);
    if (!ok) b->trunc = true;
    return ok;
}

bool sb_fmt(StrBuf *b, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool ok = sb_vfmt(b, fmt, ap);
    va_end(ap);
    return ok;
}
