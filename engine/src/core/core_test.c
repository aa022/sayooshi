/* core/core_test.c — --selftest core: unit tests of the memory model, strings, UTF coding, number parsing,
 * paths, Rd/Wr and hashes. Fatal paths (budget overrun, double free, stale handle, overflow) are exercised with a
 * fatal trap that longjmps back. Tests that would make Valgrind/ASan report on purpose (guard overwrite, reading
 * freed headers) run only when the software checks are active (plain debug builds). */
#include "core/bin.h"
#include "core/check.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/mem_debug.h"
#include "core/str.h"
#include <setjmp.h>
#include <stdio.h>
#include <string.h>

static int g_fail, g_checks;
static jmp_buf g_jmp;

#define CHECK(cond) do { g_checks++; if (!(cond)) { g_fail++; \
    log_msg(LOG_ERR, "core_selftest: %s:%d: %s", __FILE__, __LINE__, #cond); } } while (0)

static void trap(const char *msg) { (void)msg; longjmp(g_jmp, 1); }

/* Runs fn(arg) and checks that it stops with a fatal error whose message contains `want`. */
static void expect_fatal(void (*fn)(void *), void *arg, const char *want)
{
    int level = log_level();
    log_set_level(-1);
    core_set_fatal_trap(trap);
    bool fired = false;
    if (setjmp(g_jmp) == 0) fn(arg);
    else fired = true;
    core_set_fatal_trap(NULL);
    log_set_level(level);
    g_checks++;
    if (!fired || !strstr(core_fatal_message(), want)) {
        g_fail++;
        log_msg(LOG_ERR, "core_selftest: expected a fatal error containing '%s' (%s)", want,
                fired ? core_fatal_message() : "none");
    }
}

/* ------------------------------------------------------------------------------------------------ heap */

static void do_double_free(void *p) { mem_free(p); }
static void do_overflow_array(void *p) { (void)p; (void)mem_alloc_array(SIZE_MAX / 2, 4, MEM_CORE); }
static void do_huge(void *p) { (void)p; (void)mem_alloc((size_t)1 << 40, MEM_CORE); }
static void do_free_foreign(void *p) { mem_free(p); }
static char *g_overrun;
static void do_overrun_free(void *p) { g_overrun = p; g_overrun[10] = 'x'; mem_free(p); }

static void test_heap(void)
{
    size_t blocks0, bytes0, blocks1, bytes1;
    mem_heap_usage(&blocks0, &bytes0);
    char *a = mem_alloc(10, MEM_CORE);         /* never NULL: out of memory is fatal */
    mem_heap_usage(&blocks1, &bytes1);
    CHECK(blocks1 == blocks0 + 1 && bytes1 == bytes0 + 10);
    CHECK(mem_size(a) == 10);
    memset(a, 'a', 10);
    a = mem_realloc(a, 100000);
    CHECK(mem_size(a) == 100000 && a[0] == 'a' && a[9] == 'a');
    int *z = mem_alloc_array(64, sizeof(int), MEM_CORE);
    bool zero = true;
    for (int i = 0; i < 64; i++) zero = zero && z[i] == 0;
    CHECK(zero);
    char *d = mem_strdup("dup", MEM_CORE);
    CHECK(!strcmp(d, "dup"));
    CHECK(mem_check_all());
    mem_free(d);
    mem_free(z);
    mem_free(a);
    mem_free(NULL);
    expect_fatal(do_overflow_array, NULL, "overflow");
    expect_fatal(do_huge, NULL, "impossible allocation");
    _Alignas(max_align_t) uint8_t fake[256];
    memset(fake, 0, sizeof fake);
    expect_fatal(do_free_foreign, fake + 128, "not a block from mem_alloc");
    if (MEM_SOFT_CHECKS()) {                     /* Valgrind / ASan would report these accesses themselves */
        void *x = mem_alloc(32, MEM_CORE);
        mem_free(x);
        expect_fatal(do_double_free, x, "mem_free");
        expect_fatal(do_overrun_free, mem_alloc(10, MEM_CORE), "guard bytes overwritten");
        g_overrun[10] = (char)MEM_POISON_GUARD;   /* repair, then release normally */
        mem_free(g_overrun);
    }
}

/* Budgets: try allocations refuse (keeping MEM_TRY_HEADROOM free), ordinary ones past the budget are fatal. */
static const size_t MB1 = (size_t)1 << 20, TEST_BUDGET = (size_t)8 << 20;
static void do_over_budget(void *p) { (void)p; (void)mem_alloc(7 * MB1, MEM_CORE); }   /* 2 MiB live + 7 */

static void test_budget(void)
{
    mem_budget_set(MEM_CORE, TEST_BUDGET);
    CHECK(mem_try_alloc_array(TEST_BUDGET - MB1 / 2, 1, MEM_CORE) == NULL);   /* inside the headroom */
    CHECK(mem_try_alloc_array(SIZE_MAX / 2, 4, MEM_CORE) == NULL);           /* overflow: NULL, not fatal */
    uint8_t *p = mem_try_alloc_array(4 * MB1, 1, MEM_CORE);
    CHECK(p && p[0] == 0 && p[4 * MB1 - 1] == 0);
    if (p) {
        p[0] = 42;
        CHECK(mem_try_realloc_array(p, 7 * MB1, 1) == NULL && p[0] == 42);  /* refused: the old block stays */
        uint8_t *q = mem_try_realloc_array(p, 2 * MB1, 1);
        CHECK(q && q[0] == 42);
        p = q ? q : p;
        expect_fatal(do_over_budget, NULL, "heap budget of 'core' exhausted");
        mem_free(p);
    }
    mem_budget_set(MEM_CORE, 0);
    p = mem_try_alloc_array(TEST_BUDGET, 1, MEM_CORE);                       /* unlimited again */
    CHECK(p != NULL);
    mem_free(p);
}

/* ----------------------------------------------------------------------------------------------- arena */

static void do_arena_overrun(void *p) { (void)arena_alloc(p, 5000, 8); }
static void do_arena_copy(void *p) { Arena copy = *(Arena *)p; (void)arena_alloc(&copy, 1, 1); }
static void do_arena_stale(void *p)
{
    Arena *a = p;
    ArenaMark m = arena_mark(a);
    (void)arena_alloc(a, 16, 8);
    arena_reset(a);
    arena_reset_to(a, m);                        /* fine: m.used == 0 here; the next call is the stale one */
    (void)arena_alloc(a, 64, 8);
    ArenaMark late = arena_mark(a);
    arena_reset(a);
    arena_reset_to(a, late);
}
static char *g_rz;
static void do_arena_redzone(void *p)
{
    Arena *a = p;
    ArenaMark m = arena_mark(a);
    g_rz = arena_alloc(a, 8, 1);
    g_rz[8] = 1;                                 /* one byte past the allocation */
    arena_reset_to(a, m);
}

static void test_arena(void)
{
    static uint8_t buf[4096];
    Arena a;
    arena_init_buffer(&a, "test", buf, sizeof buf);
    uint64_t *u = ARENA_NEW(&a, uint64_t);
    CHECK(u && ((uintptr_t)u % _Alignof(uint64_t)) == 0 && *u == 0);
    char *c = arena_alloc(&a, 1, 1);
    double *dd = ARENA_NEW_ARRAY(&a, double, 4);
    CHECK(c && dd && ((uintptr_t)dd % _Alignof(double)) == 0 && dd[3] == 0.0);
    void *page = arena_alloc(&a, 16, 256);
    CHECK(((uintptr_t)page % 256) == 0);
    ArenaMark m = arena_mark(&a);
    size_t used = a.used;
    char *s = arena_strdup(&a, "hello");
    CHECK(!strcmp(s, "hello") && a.used > used);
    arena_reset_to(&a, m);
    CHECK(a.used == used);
    char *s2 = arena_alloc(&a, 6, 1);
    CHECK(s2 && s2[0] == 0 && s2[5] == 0);      /* zeroed again after the reset */
    CHECK(arena_try_alloc(&a, sizeof buf, 1) == NULL);
    expect_fatal(do_arena_overrun, &a, "arena 'test' out of budget: request of 5000 bytes");
    expect_fatal(do_arena_copy, &a, "copied by value");
    expect_fatal(do_arena_stale, &a, "mark above the fill level");
    if (MEM_SOFT_CHECKS()) {
        expect_fatal(do_arena_redzone, &a, "redzone");
        g_rz[8] = (char)MEM_POISON_GUARD;        /* repair, so the final reset passes */
    }
    arena_reset(&a);
    CHECK(a.used == 0 && a.peak > 0 && a.resets > 0);
    Arena child;
    arena_init_child(&child, "child", &a, 512);
    CHECK(arena_alloc(&child, 100, 16) != NULL && arena_remaining(&child) < 512);
    arena_destroy(&child);
    arena_destroy(&a);

    Arena h;
    arena_init(&h, "heap-backed", 1 << 16);
    for (int i = 0; i < 100; i++) (void)arena_alloc(&h, 100, 8);
    CHECK(h.allocs == 100 && h.used >= 10000);
    arena_destroy(&h);
}

static void test_scratch(void)
{
    Scratch s = mem_scratch_begin(NULL);
    size_t before = s.arena->used;
    char *p = arena_alloc(s.arena, 100, 1);
    Scratch inner = mem_scratch_begin(s.arena);   /* nested: must not be the caller's arena */
    CHECK(inner.arena != s.arena);
    (void)arena_alloc(inner.arena, 1000, 1);
    mem_scratch_end(&inner);
    CHECK(p[0] == 0 && s.arena->used > before);
    Arena *a = s.arena;
    mem_scratch_end(&s);
    CHECK(a->used == before && s.arena == NULL);
}

/* ------------------------------------------------------------------------------------------- cache */

static int g_freed;
static void count_free(void *v) { g_freed++; mem_free(v); }

static void test_cache(void)
{
    Cache c;
    cache_init(&c, "test", 300, 4, count_free);
    char key[16];
    for (int i = 0; i < 4; i++) {
        CHECK(str_fmt(key, sizeof key, "k%d", i));
        CHECK(cache_put(&c, key, mem_alloc(100, MEM_CORE), 100));
    }
    CHECK(c.count == 4 && c.bytes == 400 && g_freed == 0);
    CHECK(!cache_put(&c, "k0", NULL, 1));       /* duplicate key: refused, ownership stays with the caller */
    CHECK(cache_get(&c, "k0") != NULL && cache_get(&c, "nope") == NULL);
    /* table "full" (cap/2 = 16 slots here) is not reached; over budget is fixed by trim, oldest first */
    cache_trim(&c);
    CHECK(c.bytes <= 300 && g_freed == 1 && cache_get(&c, "k1") == NULL && cache_get(&c, "k0") != NULL);
    char longkey[CACHE_KEY_MAX + 2];
    memset(longkey, 'x', sizeof longkey - 1);
    longkey[sizeof longkey - 1] = 0;
    void *v = mem_alloc(1, MEM_CORE);
    CHECK(!cache_put(&c, longkey, v, 1));
    mem_free(v);
    cache_clear(&c);
    CHECK(c.count == 0 && c.bytes == 0 && g_freed == 4);
    /* fill every probe run, including the ones that wrap around the end of the table, then clear */
    g_freed = 0;
    for (int i = 0; i < 8; i++) {
        CHECK(str_fmt(key, sizeof key, "w%d", i));
        CHECK(cache_put(&c, key, mem_alloc(1, MEM_CORE), 1));
    }
    cache_clear(&c);
    int left = 0;
    for (int i = 0; i < 8; i++) {
        CHECK(str_fmt(key, sizeof key, "w%d", i));
        left += cache_get(&c, key) != NULL;
    }
    CHECK(c.count == 0 && g_freed == 8 && left == 0);
    cache_destroy(&c);
}

static void test_release_top(void)
{
    Arena a;
    arena_init(&a, "top", 4096);
    ArenaMark m0 = arena_mark(&a);
    (void)arena_alloc(&a, 100, 8);
    ArenaMark m1 = arena_mark(&a);
    CHECK(arena_release_top(&a, m0, m1) && a.used == m0.used);   /* newest: released */
    (void)arena_alloc(&a, 100, 8);
    m1 = arena_mark(&a);
    (void)arena_alloc(&a, 10, 8);                                /* something newer on top */
    CHECK(!arena_release_top(&a, m0, m1) && a.used > m1.used);
    arena_reset(&a);
    m0 = arena_mark(&a);
    (void)arena_alloc(&a, 100, 8);
    m1 = arena_mark(&a);
    arena_reset(&a);
    (void)arena_alloc(&a, 100, 8);                               /* same fill level, but someone else's memory */
    CHECK(!arena_release_top(&a, m0, m1) && a.used == m1.used);
    arena_destroy(&a);
}

/* ---------------------------------------------------------------------------------------------- strings */

static void test_str(void)
{
    char b[8];
    CHECK(str_copy_c(b, sizeof b, "1234567") && !strcmp(b, "1234567"));
    CHECK(!str_copy_c(b, sizeof b, "12345678") && !strcmp(b, "1234567"));
    CHECK(!str_copy_c(b, 6, "abcd\xe3\x81\x82") && !strcmp(b, "abcd"));   /* never cut inside a UTF-8 sequence */
    CHECK(!str_fmt(b, sizeof b, "%d-%s", 12345, "xyz") && !strcmp(b, "12345-x"));
    CHECK(str_fmt(b, sizeof b, "%zu", (size_t)42) && !strcmp(b, "42"));
    CHECK(str_cat_c(b, sizeof b, "ab") && !strcmp(b, "42ab") && !str_cat_c(b, sizeof b, "cdef") &&
          !strcmp(b, "42abcde"));
    CHECK(!str_fmt(b, sizeof b, "abcde%s", "\xe3\x81\x82") && !strcmp(b, "abcde"));   /* vsnprintf cut repaired */
    CHECK(!str_fmt(b, sizeof b, "ab%s", "\xe3\x81\x82\xe3\x81\x82") && !strcmp(b, "ab\xe3\x81\x82"));
    CHECK(str_fmt(b, sizeof b, "%s", "\xe3\x81\x82") && !strcmp(b, "\xe3\x81\x82"));
    SB_ON_STACK(sb, 10);
    CHECK(sb_put_c(&sb, "abc") && sb_fmt(&sb, "%d", 7) && sb_put_char(&sb, '!') && !strcmp(sb_cstr(&sb), "abc7!"));
    CHECK(!sb_put_c(&sb, "longer") && !sb_ok(&sb) && sb.n == 9 && !strcmp(sb_cstr(&sb), "abc7!long"));
    sb_reset(&sb);
    CHECK(sb_ok(&sb) && sb.n == 0 && sb_put_cp(&sb, 0x3042) && sb.n == 3);
    Str s = STR("  Hello, World  ");
    Str t = str_trim(s);
    size_t pos;
    CHECK(str_eq(t, STR("Hello, World")) && str_eq_fold(t, STR("hello, world")) && str_cmp(t, STR("Hello")) > 0);
    CHECK(str_find(t, STR("World"), &pos) && pos == 7 && !str_find(t, STR("xyz"), &pos));
    CHECK(str_find_char(t, 'o', &pos) && pos == 4 && str_rfind_char(t, 'o', &pos) && pos == 8);
    CHECK(str_starts_with(t, STR("Hell")) && str_ends_with_fold(t, STR("WORLD")) &&
          str_eq(str_sub(t, 7, 99), STR("World")));
    Str rest = STR("a,,b"), f;
    int n = 0;
    char got[4][4] = { "", "", "", "" };
    while (n < 4 && str_split(&rest, ',', &f)) { CHECK(str_copy(got[n], sizeof got[n], f)); n++; }
    CHECK(n == 3 && !strcmp(got[0], "a") && !strcmp(got[1], "") && !strcmp(got[2], "b"));
}

static void test_utf(void)
{
    static const struct { const char *in; size_t n; uint32_t cp; bool ok; } C[] = {
        { "A", 1, 'A', true }, { "\xc3\xa9", 2, 0xE9, true }, { "\xe3\x81\x82", 3, 0x3042, true },
        { "\xf0\x9f\x98\x80", 4, 0x1F600, true },
        { "\xc0\x80", 2, UTF_REPLACEMENT, false },         /* overlong NUL */
        { "\xe0\x80\x80", 3, UTF_REPLACEMENT, false },     /* overlong */
        { "\xed\xa0\x80", 3, UTF_REPLACEMENT, false },     /* encoded surrogate D800 */
        { "\xf4\x90\x80\x80", 4, UTF_REPLACEMENT, false }, /* > U+10FFFF */
        { "\xe3\x81", 2, UTF_REPLACEMENT, false },         /* truncated */
        { "\x80", 1, UTF_REPLACEMENT, false },             /* stray continuation */
        { "\xfe", 1, UTF_REPLACEMENT, false },
    };
    for (size_t i = 0; i < COUNT_OF(C); i++) {
        size_t pos = 0;
        uint32_t cp;
        bool ok = utf_decode8(str_make(C[i].in, C[i].n), &pos, &cp);
        CHECK(ok == C[i].ok && cp == C[i].cp && pos == (ok ? C[i].n : 1));
    }
    CHECK(utf_valid8(STR("ok \xe3\x81\x82")) && !utf_valid8(STR("bad \xc0\x80")));
    static const uint16_t pair[] = { 0xD83D, 0xDE00 }, lone_hi[] = { 0xD83D, 'x' }, lone_lo[] = { 0xDE00 };
    size_t pos = 0;
    uint32_t cp;
    CHECK(utf_decode16(pair, 2, &pos, &cp) && cp == 0x1F600 && pos == 2);
    pos = 0;
    CHECK(!utf_decode16(lone_hi, 2, &pos, &cp) && cp == UTF_REPLACEMENT && pos == 1);
    CHECK(utf_decode16(lone_hi, 2, &pos, &cp) && cp == 'x');
    pos = 0;
    CHECK(!utf_decode16(lone_lo, 1, &pos, &cp) && cp == UTF_REPLACEMENT);
    uint16_t u[2];
    char e[4];
    CHECK(utf_encode16(0x1F600, u) == 2 && u[0] == 0xD83D && u[1] == 0xDE00);
    CHECK(utf_encode16(0xD800, u) == 1 && u[0] == UTF_REPLACEMENT);
    CHECK(utf_encode8(0x110000, e) == 3 && (uint8_t)e[0] == 0xEF);
}

/* WStr: strict and legacy conversions, copies, comparison; WBuf truncation. */
static void test_wstr(void)
{
    static const uint16_t lone_hi[] = { 0xD83D, 'x' };
    WStr w = { 0 };
    CHECK(ws_from_utf8(&w, STR("a\xe3\x81\x82\xf0\x9f\x98\x80")) && w.len == 4 && w.p[1] == 0x3042 && w.p[2] == 0xD83D);
    CHECK(!ws_from_utf8(&w, STR("x\xc0\x80y")) && w.len == 4 && w.p[1] == UTF_REPLACEMENT && w.p[2] == UTF_REPLACEMENT);
    ws_set(&w, lone_hi, 2);
    char *legacy = ws_utf8(&w);                   /* lone surrogate kept as its 3-byte (WTF-8) form */
    CHECK(!strcmp(legacy, "\xed\xa0\xbdx"));
    SB_ON_STACK(sb, 16);
    CHECK(ws_to_utf8(&w, &sb) && !strcmp(sb_cstr(&sb), legacy));   /* WTF-8 too: a lone surrogate is no failure */
    SB_ON_STACK(tiny, 3);
    CHECK(!ws_to_utf8(&w, &tiny) && !sb_ok(&tiny));                /* false only on truncation */
    ws_utf8_free(legacy);
    ws_set_utf8(&w, "ab");
    WStr w2 = { 0 };
    ws_copy(&w2, &w);
    ws_cat(&w2, &w2);                             /* aliasing */
    CHECK(w2.len == 4 && w2.p[3] == 'b' && w2.p[4] == 0 && ws_cmp(&w, &w2) < 0);
    ws_set_utf8(&w, "AB");
    ws_set_utf8(&w2, "ab");
    CHECK(ws_cmp_fold(&w, &w2) == 0 && ws_cmp(&w, &w2) < 0);
    ws_free(&w);
    ws_free(&w2);
    CHECK(w.p == NULL && w.len == 0);
    uint16_t wbs[4];
    WBuf wb;
    wb_init(&wb, wbs, 4);
    CHECK(wb_put_cp(&wb, 'a') && wb_put_cp(&wb, 'b') && !wb_put_cp(&wb, 0x1F600) && wb.n == 2 && !wb_ok(&wb));
}

/* Blank-separated tokens and leading integers (input scripts, reference tables). */
static void test_tokens(void)
{
    Str rest = STR("  key   F11\tdown "), t = str_next_token(&rest);
    CHECK(str_eq(t, STR("key")));
    t = str_next_token(&rest);
    CHECK(str_eq(t, STR("F11")) && str_eq(str_next_token(&rest), STR("down")) && str_next_token(&rest).n == 0);
    int32_t v[3] = { 0, 0, 0 };
    CHECK(str_scan_ints(STR(" 12 -3\t7 x"), v, 3) == 3 && v[0] == 12 && v[1] == -3 && v[2] == 7);
    CHECK(str_scan_ints(STR("5 junk 6"), v, 3) == 1 && v[0] == 5);
    CHECK(str_eq(str_skip_blanks(STR(" \t a ")), STR("a ")));
}

static void test_parse(void)
{
    int32_t v;
    int64_t l;
    uint64_t u;
    long long r;
    CHECK(str_to_i32(STR("123"), &v) && v == 123);
    CHECK(str_to_i32(STR("-2147483648"), &v) && v == INT32_MIN);
    CHECK(!str_to_i32(STR("2147483648"), &v) && !str_to_i32(STR("12a"), &v) && !str_to_i32(STR(""), &v));
    CHECK(!str_to_i32(STR("-"), &v) && !str_to_i32(STR(" 1"), &v) && str_to_i32(STR("+7"), &v) && v == 7);
    CHECK(str_to_i64(STR("-9223372036854775808"), &l) && l == INT64_MIN && !str_to_i64(STR("9223372036854775808"), &l));
    CHECK(str_to_u64(STR("0xFFffFFffFFffFFff"), &u) && u == UINT64_MAX && !str_to_u64(STR("0x1ffffffffffffffff"), &u));
    CHECK(!str_to_u64(STR("-1"), &u) && !str_to_u64(STR("0x"), &u));
    CHECK(str_to_u64_hex(STR("cbF43926"), &u) && u == 0xCBF43926u && !str_to_u64_hex(STR("0x1"), &u) &&
          !str_to_u64_hex(STR(""), &u) && !str_to_u64_hex(STR("12g"), &u));
    CHECK(str_to_int_range(STR("8"), 1, 8, &r) && r == 8 && !str_to_int_range(STR("9"), 1, 8, &r));
    Str t = STR("(12,-3)");
    size_t pos = 1;
    CHECK(str_scan_i32(t, &pos, &v) && v == 12 && pos == 3);
    pos = 4;
    CHECK(str_scan_i32(t, &pos, &v) && v == -3 && pos == 6);
    pos = 0;
    CHECK(!str_scan_i32(t, &pos, &v) && pos == 0);
}

static void test_path(void)
{
    SB_ON_STACK(sb, 64);
    CHECK(path_join(&sb, STR("dir/"), STR("/file.txt")) && !strcmp(sb_cstr(&sb), "dir/file.txt"));
    CHECK(path_join(&sb, STR("dir"), STR("file")) && !strcmp(sb_cstr(&sb), "dir/file"));
    CHECK(str_eq(path_base(STR("a\\b/c.G00")), STR("c.G00")) && str_eq(path_dir(STR("a/b/c")), STR("a/b")));
    CHECK(str_eq(path_ext(STR("x/y.tar.gz")), STR("gz")) && path_ext(STR(".hidden")).n == 0);
    CHECK(path_normalize(&sb, STR("a\\.\\b//c/../d")) && !strcmp(sb_cstr(&sb), "a/b/d"));
    CHECK(path_normalize(&sb, STR("/x/../../y")) && !strcmp(sb_cstr(&sb), "/y"));
    CHECK(!path_normalize(&sb, STR("../etc/passwd")));
    CHECK(path_is_safe_rel(STR("ja/g00/BG.g00")) && !path_is_safe_rel(STR("../x")) && !path_is_safe_rel(STR("/x")));
    CHECK(!path_is_safe_rel(STR("C:x")) && !path_is_safe_rel(STR("a/../b")) && !path_is_safe_rel(STR("")));
}

/* ------------------------------------------------------------------------------------------- Rd, Wr, hash */

static void test_bin(void)
{
    static const uint8_t d[] = { 1, 0x34, 0x12, 0x78, 0x56, 0x34, 0x12, 0xff, 0xff, 0xff, 0xff, 3, 0, 0, 0, 9, 9, 9 };
    Rd r = rd_make(d, sizeof d);
    CHECK(rd_u8(&r) == 1 && rd_u16le(&r) == 0x1234 && rd_u32le(&r) == 0x12345678 && rd_i32le(&r) == -1);
    CHECK(rd_count(&r, 10, 1) == 3 && rd_left(&r) == 3);
    Rd sub = rd_sub(&r, 2);
    CHECK(rd_ok(&sub) && rd_u8(&sub) == 9 && rd_u8(&sub) == 9 && rd_ok(&sub));
    CHECK(rd_u8(&sub) == 0 && !rd_ok(&sub));     /* past the sub-reader's end: sticky failure */
    CHECK(rd_ok(&r) && rd_u32le(&r) == 0 && !rd_ok(&r) && rd_left(&r) == 0 && rd_u8(&r) == 0);
    r = rd_make(d, sizeof d);
    Rd at = rd_sub_at(&r, 15, 3), bad = rd_sub_at(&r, 16, 3);
    CHECK(rd_ok(&at) && rd_u8(&at) == 9 && !rd_ok(&bad) && rd_left(&bad) == 0);
    CHECK(rd_seek(&r, sizeof d) && !rd_seek(&r, sizeof d + 1) && !rd_ok(&r));
    r = rd_make(d, sizeof d);
    uint8_t out[8];
    CHECK(rd_skip(&r, 11) && rd_count(&r, 2, 1) == 0 && !rd_ok(&r));   /* 3 > max 2 */
    r = rd_make(d, 4);
    CHECK(!rd_bytes(&r, out, 5) && out[0] == 0 && rd_view(&r, 1) == NULL);
    Rd empty = rd_make(NULL, 0);
    CHECK(rd_ok(&empty) && rd_left(&empty) == 0);

    uint8_t fixed[6];
    Wr w;
    wr_init_fixed(&w, fixed, sizeof fixed);
    wr_u16le(&w, 0xBEEF);
    wr_u32le(&w, 0x01020304);
    CHECK(wr_ok(&w) && wr_len(&w) == 6 && fixed[0] == 0xEF && fixed[5] == 0x01);
    wr_u8(&w, 1);
    CHECK(!wr_ok(&w) && wr_len(&w) == 6);
    Wr hw;
    wr_init_heap(&hw, 1000);
    for (int i = 0; i < 200; i++) wr_u32le(&hw, (uint32_t)i);
    CHECK(wr_ok(&hw) && wr_len(&hw) == 800 && bin_load_le32(wr_data(&hw) + 796) == 199);
    CHECK(wr_patch_u32le(&hw, 0, 0xAABBCCDD) && wr_data(&hw)[0] == 0xDD && !wr_patch_u32le(&hw, 798, 1));
    wr_free(&hw);
    wr_init_heap(&hw, 1000);
    wr_zero(&hw, 1001);
    CHECK(!wr_ok(&hw) && wr_len(&hw) == 0);
    wr_free(&hw);
}

static void test_hash(void)
{
    CHECK(hash_crc32(0, "123456789", 9) == 0xCBF43926u);
    CHECK(hash_crc32(hash_crc32(0, "1234", 4), "56789", 5) == 0xCBF43926u && hash_crc32(0, NULL, 0) == 0);
    CHECK(hash_fnv64(HASH_FNV64_INIT, "a", 1) == 0xaf63dc4c8601ec8cull);
}

int core_selftest(void)
{
    g_fail = g_checks = 0;
    test_heap();
    test_budget();
    test_arena();
    test_scratch();
    test_cache();
    test_release_top();
    test_str();
    test_utf();
    test_wstr();
    test_parse();
    test_tokens();
    test_path();
    test_bin();
    test_hash();
    printf("  core: %d checks, %d failed (debug %s, software guard checks %s)\n", g_checks, g_fail,
           MEM_DEBUG ? "on" : "off", MEM_SOFT_CHECKS() ? "on" : "off");
    return g_fail ? 1 : 0;
}
