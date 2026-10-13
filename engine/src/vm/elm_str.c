/* vm/elm_str.c — the methods of a str element (FUN_00661620; VM §6.2): upper/lower, left/mid/right by code units
 * and by half-width length, len/cnt, search, tonum, get_code. Results are pushed straight into a new str-stack
 * entry, so no temporary strings are allocated. Every index from the script is clamped to the string. */
#include "vm/vm_internal.h"

/* Half-width = CP932 single byte (ENGINE_SPEC §1.6): ASCII and U+FF61..FF9F. */
static int hw1(uint16_t ch) { return (ch < 0x80 || (ch >= 0xFF61 && ch <= 0xFF9F)) ? 1 : 2; }

int elm_hw_len(const WStr *s)
{
    int n = 0;
    for (int i = 0; i < s->len; i++) n += hw1(s->p[i]);
    return n;
}

static uint16_t fold1(uint16_t c) { return (c >= 'A' && c <= 'Z') ? (uint16_t)(c + 32) : c; }
static uint16_t upper1(uint16_t c) { return (c >= 'a' && c <= 'z') ? (uint16_t)(c - 32) : c; }

static int find_fold(const WStr *h, const WStr *n, bool last)
{
    int res = -1;
    for (int i = 0; i + n->len <= h->len; i++) {
        int k = 0;
        while (k < n->len && fold1(h->p[i + k]) == fold1(n->p[k])) k++;
        if (k == n->len) { res = i; if (!last) break; }
    }
    return res;
}

/* s[a, a + n) clamped to s. */
static void push_sub(const WStr *s, int a, int n)
{
    if (a < 0) a = 0;
    if (a > s->len) a = s->len;
    if (n < 0) n = 0;
    if (n > s->len - a) n = s->len - a;
    WStr *d = vm_str_push_slot();
    if (d) ws_set(d, s->p ? s->p + a : NULL, n);
}

static void push_case(const WStr *s, bool upper)   /* ASCII only */
{
    WStr *d = vm_str_push_slot();
    if (!d) return;
    ws_copy(d, s);
    for (int i = 0; i < d->len; i++) d->p[i] = upper ? upper1(d->p[i]) : fold1(d->p[i]);
}

/* Units of s from `from` that fit in maxw half-width units (left_len / mid_len). */
static int hw_prefix(const WStr *s, int from, int maxw)
{
    int w = 0, i = from;
    while (i < s->len && w + hw1(s->p[i]) <= maxw) w += hw1(s->p[i++]);
    return i - from;
}

static void mid_len(const WStr *s, const ElmCall *c)   /* mid_len(start[, len]) [I] */
{
    int a = 0, w = 0, st = elm_arg_int(c, 0, 0);
    while (a < s->len && w + hw1(s->p[a]) <= st) w += hw1(s->p[a++]);
    push_sub(s, a, c->al_id == 1 ? hw_prefix(s, a, elm_arg_int(c, 1, 0)) : s->len - a);
}

static void right_len(const WStr *s, int n)    /* right_len(n) [I] */
{
    int w = 0, a = s->len;
    while (a > 0 && w + hw1(s->p[a - 1]) <= n) w += hw1(s->p[--a]);
    push_sub(s, a, s->len - a);
}

static int digit(uint16_t ch)                  /* ASCII and full-width digits */
{
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 0xFF10 && ch <= 0xFF19) return ch - 0xFF10;
    return -1;
}

static int32_t tonum(const WStr *s)            /* tonum [I]: leading blanks, sign, digits; wraps like a uint32 */
{
    int i = 0;
    uint32_t v = 0;
    bool neg = false;
    while (i < s->len && (s->p[i] == ' ' || s->p[i] == 0x3000)) i++;
    if (i < s->len && (s->p[i] == '-' || s->p[i] == 0xFF0D)) { neg = true; i++; }
    for (; i < s->len && digit(s->p[i]) >= 0; i++) v = v * 10u + (uint32_t)digit(s->p[i]);
    return (int32_t)(neg ? 0u - v : v);
}

static ElmResult str_self(WStr *s, ElmCall *c)   /* the str itself: get / assign */
{
    if (c->al_id == 0) vm_push_str(s);
    else if (c->al_id == 1 && c->arg && c->nall > 0 && c->arg[0].form == ELM_FORM_STR && &c->arg[0].s != s)
        ws_copy(s, &c->arg[0].s);
    return ELM_OK;
}

ElmResult elm_str(WStr *s, const int *rest, int nrest, ElmCall *c)
{
    if (nrest <= 0) return str_self(s, c);
    int m = rest[0];
    switch (m) {
    case 0: case 1: push_case(s, m == 0); break;                                       /* upper / lower */
    case 2: push_sub(s, 0, elm_arg_int(c, 0, 0)); break;                               /* left(n) */
    case 3: push_sub(s, elm_arg_int(c, 0, 0), c->al_id == 1 ? elm_arg_int(c, 1, 0) : s->len); break;   /* mid */
    case 4: {                                                                          /* right(n) */
        int n = elm_arg_int(c, 0, 0);
        push_sub(s, s->len - (n < 0 ? 0 : n > s->len ? s->len : n), s->len);
        break;
    }
    case 5: vm_push_int(elm_hw_len(s)); break;                                         /* len (half-width) */
    case 6: vm_push_int(s->len); break;                                                /* cnt (code units) */
    case 7: push_sub(s, 0, hw_prefix(s, 0, elm_arg_int(c, 0, 0))); break;              /* left_len(n) */
    case 8: mid_len(s, c); break;
    case 9: right_len(s, elm_arg_int(c, 0, 0)); break;
    case 10: case 11: vm_push_int(find_fold(s, elm_arg_str(c, 0), m == 11)); break;   /* search / search_last */
    case 12: vm_push_int(tonum(s)); break;
    case 13: {                                                                         /* get_code(i) [I] */
        int i = elm_arg_int(c, 0, 0);
        vm_push_int(i >= 0 && i < s->len ? s->p[i] : 0);
        break;
    }
    default:
        vm_err("str method %d stubbed", m);
        vm_push_int(0);
        break;
    }
    return ELM_OK;
}
