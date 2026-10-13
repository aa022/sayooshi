/* vm/elm_fmt.c — element calls as text, in the vm_ref.py log / trace §3.2 format: chains "42.0", argument lists
 * "[0, 'x', [1, 2]]" and Python repr() of script strings. Output goes to a caller StrBuf, which never overflows
 * (a full buffer cuts the text at a code point). */
#include "vm/vm_internal.h"

/* str.isprintable() approximation for the BMP (Cc, Cf, Zs, Zl, Zp, Cs, Co are not printable). */
static bool py_printable(unsigned c)
{
    if (c < 0x20 || (c >= 0x7F && c <= 0xA0) || c == 0xAD) return false;
    if (c == 0x1680 || (c >= 0x2000 && c <= 0x200F) || (c >= 0x2028 && c <= 0x202F)) return false;
    if ((c >= 0x205F && c <= 0x2064) || (c >= 0x2066 && c <= 0x206F) || c == 0x3000) return false;
    if ((c >= 0x0600 && c <= 0x0605) || c == 0x061C || c == 0x06DD || c == 0x070F || c == 0x180E) return false;
    if ((c >= 0xD800 && c <= 0xF8FF) || c == 0xFEFF || (c >= 0xFFF9 && c <= 0xFFFB) || c >= 0xFFFE) return false;
    return true;
}

/* One UTF-16 unit of a repr body. Surrogates are not printable, so every unit >= 0x80 written as text is a BMP
 * code point. */
static void repr_unit(StrBuf *b, unsigned c, char q)
{
    if (c == (unsigned char)q || c == '\\') { (void)sb_put_char(b, '\\'); (void)sb_put_char(b, (char)c); }
    else if (c == '\t') (void)sb_put_c(b, "\\t");
    else if (c == '\n') (void)sb_put_c(b, "\\n");
    else if (c == '\r') (void)sb_put_c(b, "\\r");
    else if (!py_printable(c)) (void)sb_fmt(b, c < 0x100 ? "\\x%02x" : "\\u%04x", c);
    else if (c < 0x80) (void)sb_put_char(b, (char)c);
    else (void)sb_put_cp(b, c);
}

void elm_repr_wstr(StrBuf *b, const WStr *s)
{
    bool sq = false, dq = false;
    for (int i = 0; i < s->len; i++) { if (s->p[i] == '\'') sq = true; if (s->p[i] == '"') dq = true; }
    char q = (sq && !dq) ? '"' : '\'';
    (void)sb_put_char(b, q);                   /* truncation is sticky in b: results are not checked */
    for (int i = 0; i < s->len && sb_ok(b); i++) repr_unit(b, s->p[i], q);
    (void)sb_put_char(b, q);
}

void elm_fmt_chain(StrBuf *b, const int *chain, int nchain)
{
    for (int i = 0; i < nchain; i++) (void)sb_fmt(b, i ? ".%d" : "%d", chain[i]);
}

/* One argument that is not an opened nested list. */
static void put_arg(StrBuf *b, const Arg *a)
{
    if (a->form == ELM_FORM_INT) (void)sb_fmt(b, "%d", a->i);
    else if (a->form == ELM_FORM_STR) elm_repr_wstr(b, &a->s);
    else if (a->form == ELM_FORM_ARGLIST) (void)sb_put_c(b, a->sub ? "[...]" : "None");   /* "[...]": too deep */
    else {
        (void)sb_fmt(b, "<elm%d:", a->form);
        elm_fmt_chain(b, a->chain, a->nchain);
        (void)sb_put_char(b, '>');
    }
}

/* Nested lists (built by read_arglist, depth <= VM_ARGLIST_DEPTH_MAX) are written depth first without recursion. */
void elm_fmt_args(StrBuf *b, const Arg *a, int cnt)
{
    typedef struct { const Arg *a; int n, i; } Level;
    Level st[VM_ARGLIST_DEPTH_MAX + 1];
    int depth = 0;
    st[0] = (Level){ a, a ? cnt : 0, 0 };
    (void)sb_put_char(b, '[');
    while (depth >= 0) {
        Level *lv = &st[depth];
        if (lv->i >= lv->n || !sb_ok(b)) { (void)sb_put_char(b, ']'); depth--; continue; }
        const Arg *x = &lv->a[lv->i];
        if (lv->i++) (void)sb_put_c(b, ", ");
        if (x->form == ELM_FORM_ARGLIST && x->sub && depth < VM_ARGLIST_DEPTH_MAX) {
            (void)sb_put_char(b, '[');
            st[++depth] = (Level){ x->sub->v, x->sub->v ? x->sub->n : 0, 0 };
        } else {
            put_arg(b, x);
        }
    }
}

void elm_fmt_call(StrBuf *b, const int *chain, int nchain, const ElmCall *c)
{
    elm_fmt_chain(b, chain, nchain);
    (void)sb_fmt(b, " al%d args=", c->al_id);
    elm_fmt_args(b, c->arg, c->nall);
    if (c->arg && c->nall > c->npos) {         /* named ids in stream order: arg[nall-1], arg[nall-2], ... */
        (void)sb_put_c(b, " named=[");
        for (int k = 0; k < c->nall - c->npos; k++) (void)sb_fmt(b, k ? ", %d" : "%d", c->arg[c->nall - 1 - k].id);
        (void)sb_put_char(b, ']');
    }
}
