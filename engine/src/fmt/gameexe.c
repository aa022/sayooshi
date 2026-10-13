/* fmt/gameexe.c — Gameexe.dat (FORMATS "Crypto", exe FUN_005408b0; AST §6).
 * File: i32 0, i32 exe_angou, body. Body: [xor crypto_exe_key] -> xor crypto_gameexe_table -> byte LZSS ->
 * UTF-16LE INI text. Every setting line is "#KEY = v1, v2, ..."; values are ints, "quoted strings" or
 * (a,b,c)(d,e,f) tuples. Range keys ("OBJECT.000-099.SAVE") are expanded into one key per number.
 *
 * Memory: the decoded text is scratch memory of gx_load. The table is built in two passes over the text — count
 * the keys, then insert them into an open-addressing table of exactly the right size — in the data arena (its
 * lifetime is the loaded language). Each line's value is stored once and shared by all keys a range expands to.
 * A reload without a data reset in between gives the previous table's memory back when it is still on top. */
#include "fmt/gameexe.h"
#include "core/bin.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "fmt/crypto.h"
#include "fmt/lzss.h"
#include "plat/fs.h"
#include <string.h>

enum {
    GX_HEADER = 8,                       /* i32 0, i32 exe_angou */
    GX_MIN_FILE = 16,
    GX_KEY_MAX = 255,                    /* longer keys are cut, as the original parser did */
    GX_MAX_KEYS = 1 << 16,               /* after range expansion; the game has ~4000 */
    GX_RANGE_SPAN = 10000,               /* hi - lo of one range */
    GX_RANGE_DEPTH = 4,                  /* ranges expanded in one key (the game uses 1) */
    BOM = 0xFEFF, IDEOGRAPHIC_SPACE = 0x3000
};

typedef struct { const char *key; const uint16_t *val; int vlen; } GxEnt;

typedef struct {
    GxEnt *tab;                          /* open addressing, power-of-two size, load <= 1/2 */
    uint32_t cap;
    int cnt, lines;
    ArenaMark start, end;
    bool loaded;
} GxTable;

static GxTable g_gx;

static uint32_t hash_key(const char *s)
{
    uint32_t h = 2166136261u;
    for (; *s; s++) h = (h ^ (uint8_t)*s) * 16777619u;
    return h;
}

static GxEnt *find_slot(const GxTable *t, const char *key)
{
    if (!t->cap) return NULL;
    uint32_t mask = t->cap - 1, i = hash_key(key) & mask;
    for (uint32_t probes = 0; probes < t->cap; probes++, i = (i + 1) & mask)   /* load <= 1/2: always ends */
        if (!t->tab[i].key || !strcmp(t->tab[i].key, key)) return &t->tab[i];
    return NULL;
}

/* ------------------------------------------------------------------------------------------------ parsing */

/* Receives every (expanded) key with its value. Pass 1 counts, pass 2 inserts. */
typedef struct GxSink {
    bool (*put)(struct GxSink *s, const char *key, const uint16_t *val, int vlen);
    Arena *arena;
    int keys;
    bool full;                           /* GX_MAX_KEYS reached or the arena is exhausted */
    const uint16_t *line_val;            /* pass 2: the arena copy of the current line's value */
} GxSink;

static bool is_sp(uint16_t c) { return c == ' ' || c == '\t' || c == '\r' || c == IDEOGRAPHIC_SPACE; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* The digit run of key around the '-' at dash, delimited by '.' or the key ends ("000-099"). */
typedef struct { size_t a, b_end; int32_t lo, hi; int width; } KeyRange;

static bool parse_range(Str key, size_t dash, KeyRange *r)
{
    size_t a = dash, be = dash + 1;
    while (a > 0 && is_digit(key.p[a - 1])) a--;
    while (be < key.n && is_digit(key.p[be])) be++;
    if (a == dash || be == dash + 1) return false;
    if ((a != 0 && key.p[a - 1] != '.') || (be != key.n && key.p[be] != '.')) return false;
    if (!str_to_i32(str_sub(key, a, dash - a), &r->lo) || !str_to_i32(str_sub(key, dash + 1, be - dash - 1), &r->hi))
        return false;                                         /* absurd digit runs are not ranges */
    r->a = a;
    r->b_end = be;
    r->width = (int)(dash - a);
    return r->lo <= r->hi && (int64_t)r->hi - r->lo < GX_RANGE_SPAN;
}

/* Up to GX_RANGE_DEPTH ranges of key, left to right (a range is never adjacent to another one). */
static int find_ranges(Str key, KeyRange *r)
{
    int n = 0;
    size_t from = 0, rel;
    while (n < GX_RANGE_DEPTH && str_find_char(str_sub(key, from, key.n), '-', &rel)) {
        size_t dash = from + rel;
        if (parse_range(key, dash, &r[n])) { from = r[n].b_end; n++; }
        else from = dash + 1;
    }
    return n;
}

/* key with ranges r[0..n) replaced by the numbers cur[0..n) (each at its range's digit width). */
static bool format_key(StrBuf *k, Str key, const KeyRange *r, const int32_t *cur, int n)
{
    size_t at = 0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        ok = sb_put(k, str_sub(key, at, r[i].a - at)) && sb_fmt(k, "%0*ld", r[i].width, (long)cur[i]) && ok;
        at = r[i].b_end;
    }
    return sb_put(k, str_sub(key, at, key.n)) && ok;
}

/* "OBJECT.000-099.SAVE" -> OBJECT.000.SAVE ... OBJECT.099.SAVE: every combination of the key's ranges, counted
 * like an odometer (the first range is the slowest). A key that would exceed GX_KEY_MAX is dropped. */
static void put_expanded(GxSink *s, Str key, const uint16_t *val, int vlen)
{
    KeyRange r[GX_RANGE_DEPTH];
    int32_t cur[GX_RANGE_DEPTH] = {0};
    int n = find_ranges(key, r);
    for (int i = 0; i < n; i++) cur[i] = r[i].lo;
    for (;;) {                                                /* ends: the odometer wraps or the sink is full */
        SB_ON_STACK(k, GX_KEY_MAX + 1);
        if (format_key(&k, key, r, cur, n) && !s->put(s, sb_cstr(&k), val, vlen)) s->full = true;
        int i = n - 1;
        while (i >= 0 && cur[i] == r[i].hi) { cur[i] = r[i].lo; i--; }
        if (i < 0 || s->full) return;
        cur[i]++;
    }
}

/* One line: "#KEY = value". Keys are upper-cased ASCII ('?' for anything else) and cut at GX_KEY_MAX. */
static void parse_line(GxSink *s, const uint16_t *u, int n, int *lines)
{
    while (n && is_sp(*u)) { u++; n--; }
    while (n && is_sp(u[n - 1])) n--;
    if (!n || u[0] != '#') return;
    int eq = 1;
    while (eq < n && u[eq] != '=') eq++;
    if (eq == n) return;
    int ke = eq;
    while (ke > 1 && is_sp(u[ke - 1])) ke--;
    char key[GX_KEY_MAX + 1];
    int kl = 0;
    for (int i = 1; i < ke && kl < GX_KEY_MAX; i++) {
        uint16_t c = u[i];
        if (c >= 'a' && c <= 'z') c = (uint16_t)(c - 32);
        key[kl++] = (char)(c < 0x80 ? c : '?');
    }
    if (!kl) return;
    const uint16_t *v = u + eq + 1;
    int vl = n - eq - 1;
    while (vl && is_sp(*v)) { v++; vl--; }
    (*lines)++;
    s->line_val = NULL;
    put_expanded(s, str_make(key, (size_t)kl), v, vl);
}

/* Splits the text into lines (a BOM at the start is skipped). Returns the number of setting lines. */
static int parse_text(GxSink *s, const uint16_t *u, int nu)
{
    int lines = 0, st = (nu && u[0] == BOM) ? 1 : 0;
    for (int i = st; i <= nu && !s->full; i++) {
        if (i == nu || u[i] == '\n') {
            parse_line(s, u + st, i - st, &lines);
            st = i + 1;
        }
    }
    return lines;
}

static bool count_key(GxSink *s, const char *key, const uint16_t *val, int vlen)
{
    (void)key; (void)val; (void)vlen;
    return ++s->keys <= GX_MAX_KEYS;
}

/* Pass 2: a later line with the same key overrides an earlier one. */
static bool insert_key(GxSink *s, const char *key, const uint16_t *val, int vlen)
{
    if (++s->keys > GX_MAX_KEYS) return false;                /* the same cut as the counting pass */
    if (!s->line_val) {                                       /* first key of this line: store the value once */
        uint16_t *v = arena_try_alloc(s->arena, ((size_t)vlen + 1) * sizeof *v, _Alignof(uint16_t));
        if (!v) return false;
        if (vlen) memcpy(v, val, (size_t)vlen * sizeof *v);
        s->line_val = v;
    }
    GxEnt *e = find_slot(&g_gx, key);
    if (!e) return false;
    if (!e->key) {
        size_t kn = strlen(key) + 1;
        char *k = arena_try_alloc(s->arena, kn, 1);
        if (!k) return false;
        memcpy(k, key, kn);
        e->key = k;
        g_gx.cnt++;
    }
    e->val = s->line_val;
    e->vlen = vlen;
    return true;
}

/* ------------------------------------------------------------------------------------------------ loading */

static void gx_clear(void)
{
    if (g_gx.loaded) (void)arena_release_top(mem_data(), g_gx.start, g_gx.end);   /* else: next data reset */
    memset(&g_gx, 0, sizeof g_gx);
}

void gx_unload(void) { gx_clear(); }

/* Decrypts a copy of the body and decodes it to UTF-16 units (scratch arena of the caller). */
static uint16_t *decode_text(Arena *a, const uint8_t *d, size_t n, int *nu)
{
    Rd r = rd_make(d, n);
    (void)rd_u32le(&r);
    int32_t exe_angou = rd_i32le(&r);
    uint8_t *body = arena_try_alloc(a, n - GX_HEADER, 1);
    if (!rd_ok(&r) || !body) return NULL;
    memcpy(body, d + GX_HEADER, n - GX_HEADER);
    if (exe_angou) crypto_xor_cycle(body, n - GX_HEADER, crypto_exe_key, sizeof crypto_exe_key, 0);
    crypto_xor_cycle(body, n - GX_HEADER, crypto_gameexe_table, sizeof crypto_gameexe_table, 0);
    uint32_t len = 0;
    const uint8_t *t = lzss_decode_arena(a, body, n - GX_HEADER, &len);
    uint16_t *u = t ? arena_try_alloc(a, ((size_t)len / 2 + 1) * sizeof *u, _Alignof(uint16_t)) : NULL;
    if (!u) return NULL;
    *nu = (int)(len / 2);                                     /* len <= LZSS_MAX_UNPACKED */
    for (int i = 0; i < *nu; i++) u[i] = bin_load_le16(t + 2 * (size_t)i);
    return u;
}

static bool build_table(const uint16_t *u, int nu)
{
    Arena *a = mem_data();
    GxSink count = { count_key, a, 0, false, NULL };
    (void)parse_text(&count, u, nu);
    if (count.full) log_msg(LOG_WARN, "gameexe: more than %d keys; the rest is ignored", GX_MAX_KEYS);
    g_gx.start = arena_mark(a);
    uint32_t cap = 16;
    while (cap < 2u * (uint32_t)count.keys) cap *= 2;
    g_gx.tab = arena_try_alloc(a, (size_t)cap * sizeof *g_gx.tab, _Alignof(GxEnt));
    if (!g_gx.tab) return false;
    g_gx.cap = cap;
    GxSink ins = { insert_key, a, 0, false, NULL };
    g_gx.lines = parse_text(&ins, u, nu);
    g_gx.end = arena_mark(a);
    g_gx.loaded = true;
    return !ins.full || count.full;                           /* insert_key fails only when the arena is full */
}

bool gx_load_mem(const uint8_t *data, size_t n, const char *what)
{
    gx_clear();
    if (!data || n < GX_MIN_FILE) {
        log_msg(LOG_ERR, "gameexe: %s is truncated", what ? what : "Gameexe.dat");
        return false;
    }
    Scratch s = mem_scratch_begin(mem_data());
    int nu = 0;
    const uint16_t *u = decode_text(s.arena, data, n, &nu);
    bool ok = u && build_table(u, nu);
    mem_scratch_end(&s);
    if (!u) log_msg(LOG_ERR, "gameexe: decode failed");
    else if (!ok) log_msg(LOG_ERR, "gameexe: data arena exhausted");
    if (!ok) gx_clear();
    log_msg(LOG_DEBUG, "gameexe: %d setting lines, %d keys", g_gx.lines, g_gx.cnt);
    return ok && g_gx.lines > 0;
}

bool gx_load(const char *gameexe_dat_path)
{
    gx_clear();
    size_t n = 0;
    uint8_t *d = fs_read(gameexe_dat_path, &n);
    if (!d) return false;
    bool ok = gx_load_mem(d, n, gameexe_dat_path);
    fs_free(d);
    return ok;
}

/* ------------------------------------------------------------------------------------------------ lookups */

static const GxEnt *get(const char *key)
{
    if (!key || !g_gx.cap) return NULL;
    if (*key == '#') key++;
    char k[GX_KEY_MAX + 1];
    int i = 0;
    for (; i < GX_KEY_MAX && key[i]; i++) k[i] = str_ascii_upper(key[i]);
    k[i] = 0;
    const GxEnt *e = find_slot(&g_gx, k);
    return e && e->key ? e : NULL;
}

bool gx_has(const char *key) { return get(key) != NULL; }

/* Field idx of a value: top-level comma split, respecting "..." and (...). Trimmed bounds in *b, *en. */
static bool field(const GxEnt *e, int idx, int *b, int *en)
{
    if (!e || idx < 0) return false;
    const uint16_t *v = e->val;
    int n = e->vlen, f = 0, st = 0, depth = 0;
    bool q = false;
    for (int i = 0; i <= n; i++) {
        uint16_t c = i < n ? v[i] : ',';
        if (q) { if (c == '"') q = false; if (i < n) continue; }
        if (c == '"') q = true;
        else if (c == '(') depth++;
        else if (c == ')' && depth) depth--;
        else if (c == ',' && (depth == 0 || i == n)) {
            if (f == idx) {
                int x = st, y = i;
                while (x < y && is_sp(v[x])) x++;
                while (y > x && is_sp(v[y - 1])) y--;
                *b = x; *en = y;
                return true;
            }
            f++;
            st = i + 1;
        }
    }
    return false;
}

int gx_nfields(const char *key)
{
    const GxEnt *e = get(key);
    int b, en, k = 0;
    while (e && field(e, k, &b, &en)) k++;
    return k;
}

/* Decimal with optional sign, saturated to [-2^31 .. 2^31-1] like the original reader. */
static bool parse_int(const uint16_t *s, int n, int *out)
{
    int i = 0;
    bool neg = false;
    if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
    if (i >= n) return false;
    long long v = 0;
    for (; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 0x7FFFFFFFLL + 1) v = 0x7FFFFFFFLL + 1;
    }
    v = neg ? -v : v;
    if (v > 0x7FFFFFFF) v = 0x7FFFFFFF;
    *out = (int)v;
    return true;
}

int gx_int(const char *key, int idx, int def)
{
    const GxEnt *e = get(key);
    int b, en, v;
    if (!field(e, idx, &b, &en)) return def;
    return parse_int(e->val + b, en - b, &v) ? v : def;
}

bool gx_str(const char *key, int idx, WStr *out)
{
    const GxEnt *e = get(key);
    int b, en;
    if (!field(e, idx, &b, &en)) return false;
    if (en - b >= 2 && e->val[b] == '"' && e->val[en - 1] == '"') { b++; en--; }
    ws_set(out, e->val + b, en - b);
    return true;
}

int gx_tuples(const char *key, int *out, int max_ints)
{
    const GxEnt *e = get(key);
    if (!e) return 0;
    int cnt = 0, depth = 0, st = 0;
    for (int i = 0; i < e->vlen; i++) {
        uint16_t c = e->val[i];
        if (c == '(') { depth = 1; st = i + 1; }
        else if (depth && (c == ',' || c == ')')) {
            int x = st, y = i, v;
            while (x < y && is_sp(e->val[x])) x++;
            while (y > x && is_sp(e->val[y - 1])) y--;
            if (y > x && parse_int(e->val + x, y - x, &v)) {
                if (cnt < max_ints && out) out[cnt] = v;
                cnt++;
            }
            st = i + 1;
            if (c == ')') depth = 0;
        }
    }
    return cnt < max_ints ? cnt : max_ints;
}

int gx_line_count(void) { return g_gx.lines; }
