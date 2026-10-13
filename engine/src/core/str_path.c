/* core/str_path.c — '/'-separated path helpers. Input may use '\\'; output always uses '/'. */
#include "core/str.h"

enum { PATH_MAX_SEGMENTS = 256 };

static bool is_sep(char c) { return c == '/' || c == '\\'; }

bool path_join(StrBuf *out, Str dir, Str name)
{
    sb_reset(out);
    bool ok = sb_put(out, dir);
    if (dir.n && !is_sep(dir.p[dir.n - 1]) && name.n) ok = sb_put_char(out, '/') && ok;
    while (name.n && is_sep(name.p[0]) && dir.n) name = str_sub(name, 1, name.n);
    return sb_put(out, name) && ok;
}

Str path_base(Str p)
{
    for (size_t i = p.n; i > 0; i--)
        if (is_sep(p.p[i - 1])) return str_sub(p, i, p.n);
    return p;
}

Str path_dir(Str p)
{
    for (size_t i = p.n; i > 0; i--)
        if (is_sep(p.p[i - 1])) return str_sub(p, 0, i - 1);
    return str_sub(p, 0, 0);
}

Str path_ext(Str p)
{
    Str b = path_base(p);
    size_t dot;
    if (!str_rfind_char(b, '.', &dot) || dot == 0) return str_sub(b, b.n, 0);
    return str_sub(b, dot + 1, b.n);
}

/* Splits p into segments without "" and "."; resolves "..". *climbed: a ".." went above the start. */
static size_t segments(Str p, Str *seg, bool *climbed)
{
    size_t n = 0, i = 0;
    *climbed = false;
    while (i < p.n) {
        size_t j = i;
        while (j < p.n && !is_sep(p.p[j])) j++;
        Str s = str_sub(p, i, j - i);
        i = j + 1;
        if (s.n == 0 || str_eq(s, STR("."))) continue;
        if (str_eq(s, STR(".."))) { if (n) n--; else *climbed = true; continue; }
        if (n == PATH_MAX_SEGMENTS) { *climbed = true; return n; }   /* absurd depth: refuse like a climb */
        seg[n++] = s;
    }
    return n;
}

bool path_normalize(StrBuf *out, Str p)
{
    Str seg[PATH_MAX_SEGMENTS];
    bool climbed, absolute = p.n && is_sep(p.p[0]);
    size_t n = segments(p, seg, &climbed);
    sb_reset(out);
    if (climbed && !absolute) return false;
    bool ok = !absolute || sb_put_char(out, '/');
    for (size_t k = 0; k < n; k++) {
        if (k) ok = sb_put_char(out, '/') && ok;
        ok = sb_put(out, seg[k]) && ok;
    }
    return ok;
}

bool path_is_safe_rel(Str p)
{
    if (p.n == 0 || is_sep(p.p[0])) return false;
    if (p.n >= 2 && p.p[1] == ':') return false;                 /* drive letter */
    for (size_t i = 0; i < p.n; i++)
        if (p.p[i] == 0) return false;
    Str rest = p, seg;
    while (rest.n) {
        size_t j = 0;
        while (j < rest.n && !is_sep(rest.p[j])) j++;
        seg = str_sub(rest, 0, j);
        if (str_eq(seg, STR(".."))) return false;
        rest = str_sub(rest, j < rest.n ? j + 1 : j, rest.n);
    }
    return true;
}
