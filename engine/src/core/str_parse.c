/* core/str_parse.c — integer parsing with full-string and range checks (replaces atoi / strtol misuse). */
#include "core/str.h"
#include <limits.h>

static int digit_val(char c, int base)
{
    int v = c >= '0' && c <= '9'   ? c - '0'
            : c >= 'a' && c <= 'f' ? c - 'a' + 10
            : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                   : 99;
    return v < base ? v : -1;
}

/* Unsigned magnitude of s[*pos..] in base; at least one digit; stops at the first non-digit. */
static bool scan_mag(Str s, size_t *pos, int base, uint64_t *out)
{
    size_t i = *pos;
    uint64_t v = 0;
    int d;
    if (i >= s.n || digit_val(s.p[i], base) < 0) return false;
    for (; i < s.n && (d = digit_val(s.p[i], base)) >= 0; i++) {
        if (v > (UINT64_MAX - (uint64_t)d) / (uint64_t)base) return false;   /* overflow */
        v = v * (uint64_t)base + (uint64_t)d;
    }
    *pos = i;
    *out = v;
    return true;
}

/* Optional sign + decimal magnitude starting at *pos, as a value in [INT64_MIN, INT64_MAX]. */
static bool scan_i64(Str s, size_t *pos, int64_t *out)
{
    size_t i = *pos;
    bool neg = false;
    if (i < s.n && (s.p[i] == '-' || s.p[i] == '+')) { neg = s.p[i] == '-'; i++; }
    uint64_t m;
    if (!scan_mag(s, &i, 10, &m)) return false;
    if (neg ? m > (uint64_t)INT64_MAX + 1u : m > (uint64_t)INT64_MAX) return false;
    *out = neg ? (m == (uint64_t)INT64_MAX + 1u ? INT64_MIN : -(int64_t)m) : (int64_t)m;
    *pos = i;
    return true;
}

bool str_to_i64(Str s, int64_t *out)
{
    size_t pos = 0;
    int64_t v;
    if (!scan_i64(s, &pos, &v) || pos != s.n) return false;
    *out = v;
    return true;
}

bool str_to_u64(Str s, uint64_t *out)
{
    size_t pos = 0;
    int base = 10;
    if (s.n > 2 && s.p[0] == '0' && (s.p[1] == 'x' || s.p[1] == 'X')) { base = 16; pos = 2; }
    uint64_t v;
    if (!scan_mag(s, &pos, base, &v) || pos != s.n) return false;
    *out = v;
    return true;
}

bool str_to_u64_hex(Str s, uint64_t *out)
{
    size_t pos = 0;
    uint64_t v;
    if (!scan_mag(s, &pos, 16, &v) || pos != s.n) return false;
    *out = v;
    return true;
}

bool str_to_int_range(Str s, long long lo, long long hi, long long *out)
{
    int64_t v;
    if (!str_to_i64(s, &v) || v < lo || v > hi) return false;
    *out = (long long)v;
    return true;
}

bool str_to_i32(Str s, int32_t *out)
{
    long long v;
    if (!str_to_int_range(s, INT32_MIN, INT32_MAX, &v)) return false;
    *out = (int32_t)v;
    return true;
}

bool str_scan_i32(Str s, size_t *pos, int32_t *out)
{
    size_t i = *pos;
    int64_t v;
    if (!scan_i64(s, &i, &v) || v < INT32_MIN || v > INT32_MAX) return false;
    *out = (int32_t)v;
    *pos = i;
    return true;
}

Str str_skip_blanks(Str s)
{
    size_t i = 0;
    while (i < s.n && (s.p[i] == ' ' || s.p[i] == '\t')) i++;
    return str_sub(s, i, s.n - i);
}

Str str_next_token(Str *rest)
{
    Str t = str_skip_blanks(*rest);
    size_t n = 0;
    while (n < t.n && t.p[n] != ' ' && t.p[n] != '\t') n++;
    *rest = str_sub(t, n, t.n - n);
    return str_sub(t, 0, n);
}

int str_scan_ints(Str s, int32_t *v, int want)
{
    size_t pos = 0;
    int got = 0;
    while (got < want) {                       /* each round consumes input or stops */
        Str rest = str_skip_blanks(str_sub(s, pos, s.n - pos));
        pos = (size_t)(rest.p - s.p);
        int32_t x;
        if (!str_scan_i32(s, &pos, &x)) break;
        v[got++] = x;
    }
    return got;
}
