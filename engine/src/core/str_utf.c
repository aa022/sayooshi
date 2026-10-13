/* core/str_utf.c — strict UTF-8 / UTF-16 coding (RFC 3629, Unicode §3.9): overlong forms, encoded surrogates,
 * code points above U+10FFFF, truncated sequences and lone surrogates all decode to U+FFFD. */
#include "core/str.h"

static bool is_surrogate(uint32_t cp) { return cp >= 0xD800 && cp <= 0xDFFF; }
static bool is_cont(unsigned char c) { return (c & 0xC0) == 0x80; }

bool utf_decode8(Str s, size_t *pos, uint32_t *cp)
{
    static const uint32_t MIN_CP[4] = { 0, 0x80, 0x800, 0x10000 };
    size_t i = *pos;
    if (i >= s.n) { *cp = UTF_REPLACEMENT; return false; }
    unsigned char c = (unsigned char)s.p[i];
    size_t more;
    uint32_t v;
    if (c < 0x80) { *cp = c; *pos = i + 1; return true; }
    else if (c >= 0xC2 && c <= 0xDF) { more = 1; v = c & 0x1Fu; }
    else if (c >= 0xE0 && c <= 0xEF) { more = 2; v = c & 0x0Fu; }
    else if (c >= 0xF0 && c <= 0xF4) { more = 3; v = c & 0x07u; }
    else { *cp = UTF_REPLACEMENT; *pos = i + 1; return false; }   /* continuation byte, C0/C1, F5..FF */
    if (more > s.n - i - 1) { *cp = UTF_REPLACEMENT; *pos = i + 1; return false; }
    for (size_t k = 1; k <= more; k++) {
        unsigned char b = (unsigned char)s.p[i + k];
        if (!is_cont(b)) { *cp = UTF_REPLACEMENT; *pos = i + 1; return false; }
        v = v << 6 | (b & 0x3Fu);
    }
    if (v < MIN_CP[more] || v > UTF_MAX_CP || is_surrogate(v)) { *cp = UTF_REPLACEMENT; *pos = i + 1; return false; }
    *cp = v;
    *pos = i + 1 + more;
    return true;
}

bool utf_decode16(const uint16_t *s, size_t n, size_t *pos, uint32_t *cp)
{
    size_t i = *pos;
    if (i >= n) { *cp = UTF_REPLACEMENT; return false; }
    uint32_t u = s[i];
    if (!is_surrogate(u)) { *cp = u; *pos = i + 1; return true; }
    if (u <= 0xDBFF && i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
        *cp = 0x10000 + ((u - 0xD800) << 10) + (uint32_t)(s[i + 1] - 0xDC00);
        *pos = i + 2;
        return true;
    }
    *cp = UTF_REPLACEMENT;   /* lone high or low surrogate */
    *pos = i + 1;
    return false;
}

size_t utf_encode8(uint32_t cp, char out[4])
{
    if (cp > UTF_MAX_CP || is_surrogate(cp)) cp = UTF_REPLACEMENT;
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

size_t utf_encode16(uint32_t cp, uint16_t out[2])
{
    if (cp > UTF_MAX_CP || is_surrogate(cp)) cp = UTF_REPLACEMENT;
    if (cp < 0x10000) { out[0] = (uint16_t)cp; return 1; }
    cp -= 0x10000;
    out[0] = (uint16_t)(0xD800 + (cp >> 10));
    out[1] = (uint16_t)(0xDC00 + (cp & 0x3FF));
    return 2;
}

bool utf_valid8(Str s)
{
    size_t pos = 0;
    uint32_t cp;
    while (pos < s.n)
        if (!utf_decode8(s, &pos, &cp)) return false;
    return true;
}
