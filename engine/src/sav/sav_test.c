/* sav/sav_test.c — save selftest (--selftest save): stream unit tests, golden encoding (tests/ref/save_golden.hex),
 * module and snapshot round trips, file fallbacks, export/import, the language rule, format-1 slots, fuzzing.
 *
 * The fuzzers are in sav_fuzz.c. Environment: SAYO_SAVE_CORPUS (';'-separated extra files, format-1 slots among
 * them are load-tested), SAYOOSHI_REF_DIR, SAYO_SAVE_GOLDEN_WRITE, SAYO_SAVE_KEEP_TMP. */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "txt/lang.h"
#include "plat/sysdep.h"
#include "vm/vm.h"
#include "vm/sysstate.h"
#include "gfx/gfx.h"
#include "gfx/canvas.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

static int st_fail;
#define ST_CHECK(cond)                                                                                                 \
    do {                                                                                                               \
        if (!(cond)) {                                                                                                 \
            st_fail++;                                                                                                 \
            log_msg(LOG_ERR, "save selftest: %s:%d: %s", __FILE__, __LINE__, #cond);                                   \
        }                                                                                                              \
    } while (0)

/* A tracked heap copy of some bytes. */
typedef struct { uint8_t *p; size_t n; } Blob;
static Blob blob_dup(const uint8_t *p, size_t n) { Blob b = { mem_dup(p, n, MEM_SAV), n }; return b; }
static void blob_free(Blob *b) { mem_free(b->p); b->p = NULL; b->n = 0; }

static Blob blob_of(const SavWriter *w)
{
    size_t n = 0;
    const uint8_t *d = sav_writer_data(w, &n);
    Blob b = { NULL, 0 };
    if (d) b = blob_dup(d, n);
    return b;
}

static Blob blob_take(SavWriter *w)   /* copy and free the writer */
{
    Blob b = blob_of(w);
    sav_writer_free(w);
    return b;
}

/* ================================================================== stream unit tests */

static const uint16_t JP4[] = { 0x3055, 0x3088, 0x306a, 0x3089 };   /* さよなら */

static SavWriter *rw_sample(const WStr *s)
{
    SavWriter *w = sav_writer_new();
    sav_put_chunk(w, FOURCC('A', 'A', 'A', 'A'));
    sav_put_i32(w, -5); sav_put_u8(w, 200); sav_put_i64(w, -1234567890123LL); sav_put_wstr(w, s);
    sav_put_chunk(w, FOURCC('B', 'B', 'B', 'B'));
    sav_put_enter(w);
    sav_put_chunk(w, FOURCC('C', 'C', 'C', 'C')); sav_put_i32(w, 7);
    sav_put_chunk(w, FOURCC('D', 'D', 'D', 'D')); sav_put_i32(w, 8); sav_put_i32(w, 9);
    sav_put_leave(w);
    sav_put_chunk(w, FOURCC('E', 'E', 'E', 'E')); sav_put_i32(w, 42);
    return w;
}

static void rw_find(const uint8_t *d, size_t len, const WStr *s)
{
    WStr t = { 0 };
    SavReader *r = sav_reader_new(d, len);
    /* out of order: E first, then nested D, C, then A */
    ST_CHECK(sav_find_chunk(r, FOURCC('E', 'E', 'E', 'E')) && sav_get_i32(r) == 42);
    ST_CHECK(sav_remaining(r) == 0 && !sav_has_more(r));
    ST_CHECK(sav_find_chunk(r, FOURCC('B', 'B', 'B', 'B')) && sav_get_enter(r));
    ST_CHECK(sav_find_chunk(r, FOURCC('D', 'D', 'D', 'D')) && sav_get_i32(r) == 8 && sav_get_i32(r) == 9);
    ST_CHECK(sav_find_chunk(r, FOURCC('C', 'C', 'C', 'C')) && sav_get_i32(r) == 7);
    sav_get_leave(r);
    ST_CHECK(sav_ok(r));
    ST_CHECK(sav_find_chunk(r, FOURCC('A', 'A', 'A', 'A')));
    ST_CHECK(sav_get_i32(r) == -5 && sav_get_u8(r) == 200 && sav_get_i64(r) == -1234567890123LL);
    ST_CHECK(sav_get_wstr(r, &t) && ws_cmp(s, &t) == 0);
    ST_CHECK(sav_ok(r));
    (void)sav_get_i32(r);   /* short read inside a chunk fails and stays failed */
    ST_CHECK(!sav_ok(r) && sav_get_u32(r) == 0 && sav_remaining(r) == 0);
    sav_reader_free(r);
    ws_free(&t);
}

static void rw_iterate(const uint8_t *d, size_t len)
{
    SavReader *r = sav_reader_new(d, len);
    uint32_t tag, seen[4];
    int k = 0;
    while (k < 4 && sav_next_chunk(r, &tag)) seen[k++] = tag;
    ST_CHECK(k == 3 && seen[0] == FOURCC('A', 'A', 'A', 'A') && seen[1] == FOURCC('B', 'B', 'B', 'B') &&
             seen[2] == FOURCC('E', 'E', 'E', 'E'));
    sav_reader_free(r);

    r = sav_reader_new(d, len);
    ST_CHECK(!sav_find_chunk(r, FOURCC('Z', 'Z', 'Z', 'Z')));
    ST_CHECK(sav_ok(r));            /* a missing chunk alone is not an error ... */
    (void)sav_get_i32(r);
    ST_CHECK(!sav_ok(r));           /* ... but reading it is */
    sav_reader_free(r);

    r = sav_reader_new(d, len - 3);   /* truncated: the last chunk is not found */
    ST_CHECK(!sav_find_chunk(r, FOURCC('E', 'E', 'E', 'E')));
    ST_CHECK(sav_find_chunk(r, FOURCC('A', 'A', 'A', 'A')));
    sav_reader_free(r);

    Blob bad = blob_dup(d, len);   /* a flipped bit in a body is caught by the chunk CRC */
    bad.p[SAV_HDR2 + 1] ^= 0x10;
    r = sav_reader_new(bad.p, bad.n);
    ST_CHECK(!sav_find_chunk(r, FOURCC('A', 'A', 'A', 'A')));
    ST_CHECK(sav_find_chunk(r, FOURCC('E', 'E', 'E', 'E')) && sav_get_i32(r) == 42);
    sav_reader_free(r);
    blob_free(&bad);
}

static void rw_rle(void)
{
    int32_t a[300];
    memset(a, 0, sizeof a);
    a[3] = 5; a[4] = -1; a[6] = 9; a[150] = 77; a[299] = 1;
    SavWriter *w = sav_writer_new();
    sav_put_chunk(w, FOURCC('R', 'L', 'E', ' '));
    sav_put_i32_rle(w, a, 300);
    sav_put_i32_rle(w, a, 0);
    size_t len;
    const uint8_t *d = sav_writer_data(w, &len);
    ST_CHECK(len < 120);
    SavReader *r = sav_reader_new(d, len);
    Scratch sc = mem_scratch_begin(NULL);
    int32_t *b = NULL;
    int bn = 0;
    ST_CHECK(sav_find_chunk(r, FOURCC('R', 'L', 'E', ' ')) && sav_get_i32_rle(r, sc.arena, &b, &bn, 1000) &&
             bn == 300 && !memcmp(a, b, sizeof a));
    ST_CHECK(sav_get_i32_rle(r, sc.arena, &b, &bn, 1000) && bn == 0 && sav_remaining(r) == 0);
    sav_reader_free(r);
    r = sav_reader_new(d, len);
    ST_CHECK(sav_find_chunk(r, FOURCC('R', 'L', 'E', ' ')) && !sav_get_i32_rle(r, sc.arena, &b, &bn, 299) &&
             !b); /* over cap */
    sav_reader_free(r);
    sav_writer_free(w);
    static const uint8_t hostile[] = { 10, 0, 0, 0,  0xff, 0xff, 0xff, 0xff,  0, 0, 0, 0 };   /* zero run 2^32-1 */
    r = sav_reader_new(hostile, sizeof hostile);
    ST_CHECK(!sav_get_i32_rle(r, sc.arena, &b, &bn, 100) && !sav_ok(r) && !b);
    sav_reader_free(r);
    static const uint8_t overrun[] = { 2, 0, 0, 0,  2, 0, 0, 0,  1, 0, 0, 0,  7, 0, 0, 0 };   /* zeros fill n, then 1 */
    r = sav_reader_new(overrun, sizeof overrun);
    ST_CHECK(!sav_get_i32_rle(r, sc.arena, &b, &bn, 100) && !sav_ok(r) && !b);
    sav_reader_free(r);
    static const uint8_t cnt[] = { 0xff, 0xff, 0xff, 0x7f };
    r = sav_reader_new(cnt, sizeof cnt);
    ST_CHECK(sav_get_count(r, 1 << 30, 4) == 0 && !sav_ok(r));
    sav_reader_free(r);
    mem_scratch_end(&sc);
}

/* str8/str16 limits: exact fits, one too long, hostile lengths. */
static void rw_strings(void)
{
    SavWriter *w = sav_writer_new();
    sav_put_chunk(w, FOURCC('S', 'T', 'R', '8'));
    sav_put_str8(w, "abc"); sav_put_str8(w, "abcd"); sav_put_str8(w, "");
    size_t len;
    const uint8_t *d = sav_writer_data(w, &len);
    SavReader *r = sav_reader_new(d, len);
    char out[4];
    Str v;
    ST_CHECK(sav_find_chunk(r, FOURCC('S', 'T', 'R', '8')) && sav_get_str8(r, out, sizeof out) && !strcmp(out, "abc"));
    ST_CHECK(sav_get_str8_view(r, &v) && str_eq(v, STR("abcd")) && sav_get_str8_view(r, &v) && v.n == 0);
    sav_reader_free(r);
    r = sav_reader_new(d, len);
    ST_CHECK(sav_find_chunk(r, FOURCC('S', 'T', 'R', '8')) && sav_get_str8(r, out, sizeof out));
    ST_CHECK(!sav_get_str8(r, out, sizeof out) && out[0] == 0 && !sav_ok(r));   /* 4 bytes do not fit in 4 */
    sav_reader_free(r);
    sav_writer_free(w);
    static const uint8_t huge[] = { 0xff, 0xff, 0xff, 0xff, 'x', 0 };
    WStr s = { 0 };
    r = sav_reader_new(huge, sizeof huge);
    ST_CHECK(!sav_get_wstr(r, &s) && s.len == 0 && !sav_ok(r));
    sav_reader_free(r);
    r = sav_reader_new(huge, sizeof huge);
    ST_CHECK(!sav_get_str8_view(r, &v) && v.n == 0 && !sav_ok(r));
    sav_reader_free(r);
    ws_free(&s);
}

/* config.sav: unknown keys (however long) are ignored, values are clamped (SAVE_FORMAT.md §4). */
static void rw_config_keys(void)
{
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_CONF);
    sav_put_chunk(w, SAV_TAG_CFG2);
    sav_put_u32(w, 3);
    sav_put_str8(w, "a.key.from.a.newer.engine.that.is.much.longer.than.sixty.four.bytes.in.total"); sav_put_i32(w, 1);
    sav_put_str8(w, "volume.bgm"); sav_put_i32(w, 999);
    sav_put_str8(w, "window.scale"); sav_put_i32(w, -4);
    sav_file_end(w);
    size_t n;
    const uint8_t *d = sav_writer_data(w, &n);
    SavConfig c;
    memset(&c, 0, sizeof c);
    c.msg_speed = 33;
    ST_CHECK(sav_config_decode(d, n, &c) && c.audio.vol_bgm == 255 && c.window_scale == 1 && c.msg_speed == 33);
    sav_writer_free(w);
}

static void selftest_rw(void)
{
    WStr s = { 0 };
    ws_set(&s, JP4, 4);
    ST_CHECK(hash_crc32(0, "123456789", 9) == 0xCBF43926u);   /* the standard CRC-32 check value */
    SavWriter *w = rw_sample(&s);
    size_t len;
    const uint8_t *d = sav_writer_data(w, &len);
    rw_find(d, len, &s);
    rw_iterate(d, len);
    sav_writer_free(w);
    rw_rle();
    rw_strings();
    rw_config_keys();
    ws_free(&s);
}

/* ================================================================== golden fixture
 * The exact bytes of a fixed encoding (cross-platform / cross-endian proof). */

static void golden_config(SavWriter *w)
{
    SavConfig c;
    memset(&c, 0, sizeof c);
    c.audio.vol_all = 255; c.audio.vol_bgm = 200; c.audio.vol_koe = 180; c.audio.vol_pcm = 160; c.audio.vol_se = 140;
    c.audio.on_all = c.audio.on_bgm = c.audio.on_koe = true; c.audio.bgm_fade = true;
    c.audio.bgm_fade_vol = 192; c.msg_speed = 20; c.auto_moji_wait = 70; c.auto_min_wait = 300; c.font_shadow = 2;
    c.wheel_next = true;
    c.skip_wipe_anime = true;
    c.chrcolor = true;
    c.window_mode = 0;
    c.window_scale = 2;
    c.language = 2;
    Blob cf = blob_take(sav_config_encode(&c, "golden", 1700000000));
    sav_put_chunk(w, FOURCC('C', 'O', 'N', 'F'));
    sav_put_bytes(w, cf.p, cf.n);
    blob_free(&cf);
}

static SavWriter *golden_encode(void)
{
    WStr s = { 0 };
    ws_set(&s, JP4, 4);
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, FOURCC('T', 'E', 'S', 'T'));
    sav_put_chunk(w, FOURCC('P', 'R', 'I', 'M'));
    sav_put_u8(w, 0xA5); sav_put_i32(w, -2); sav_put_u32(w, 0xDEADBEEFu); sav_put_i64(w, -1234567890123LL);
    sav_put_u64(w, 0x0123456789ABCDEFull);
    sav_put_wstr(w, &s); sav_put_str8(w, "h\xc3\xa9llo");
    static const int32_t rle[] = { 0, 0, 0, 5, -1, 0, 0, 0, 0, 7, 0, 3 };
    sav_put_i32_rle(w, rle, 12);
    sav_put_chunk(w, FOURCC('N', 'E', 'S', 'T'));
    sav_put_enter(w);
    sav_put_chunk(w, FOURCC('A', 'A', 'A', 'A')); sav_put_i32(w, 1);
    sav_put_chunk(w, FOURCC('B', 'B', 'B', 'B')); sav_put_i32(w, 2);
    sav_put_leave(w);
    static const uint8_t px[12] = { 255, 0, 0, 0, 255, 0, 0, 0, 255, 18, 52, 86 };   /* thumbnail encoder */
    uint8_t p565[8];
    sav_thumb_to_565(px, 4, p565);
    sav_put_chunk(w, FOURCC('T', '5', '6', '5')); sav_put_bytes(w, p565, sizeof p565);
    golden_config(w);
    sav_file_end(w);
    ws_free(&s);
    return w;
}

/* tests/ref: $SAYOOSHI_REF_DIR, <this file>/../../../tests/ref (engine/tests/ref), ./tests/ref, ./engine/tests/ref. */
static const char *ref_dir(Arena *ar)
{
    const char *env = SDL_getenv("SAYOOSHI_REF_DIR");
    if (env && *env) return arena_strdup(ar, env);
    Str engine = path_dir(path_dir(path_dir(str_c(__FILE__))));
    const char *cand[3] = { NULL, "tests/ref", "engine/tests/ref" };
    if (engine.n) {
        char *e = arena_alloc(ar, engine.n + 1, 1);
        memcpy(e, engine.p, engine.n);
        cand[0] = str_cat_arena(ar, e, "/tests/ref", NULL);
    }
    for (int i = 0; i < 3; i++)
        if (cand[i] && plat_path_is_file(str_cat_arena(ar, cand[i], "/save_golden.hex", NULL))) return cand[i];
    return "tests/ref";
}

/* Lower-case hex, 32 bytes per line, every line (also the last) ended by '\n'. */
static const char *to_hex(Arena *ar, const uint8_t *d, size_t n, size_t *out_len)
{
    static const char X[] = "0123456789abcdef";
    char *h = arena_alloc_uninit(ar, n * 2 + n / 32 + 2, 1);
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        h[o++] = X[d[i] >> 4];
        h[o++] = X[d[i] & 15];
        if (i % 32 == 31 || i + 1 == n) h[o++] = '\n';
    }
    h[o] = 0;
    *out_len = o;
    return h;
}

static void selftest_golden(void)
{
    SavWriter *w = golden_encode();
    size_t n, hn;
    const uint8_t *d = sav_writer_data(w, &n);
    Scratch s = mem_scratch_begin(NULL);
    const char *hex = to_hex(s.arena, d, n, &hn);
    const char *gen = SDL_getenv("SAYO_SAVE_GOLDEN_WRITE");
    if (gen && *gen) {
        ST_CHECK(SDL_SaveFile(gen, hex, hn));
        printf("  golden fixture written to %s (%u bytes)\n", gen, (unsigned)n);
    } else {
        const char *path = str_cat_arena(s.arena, ref_dir(s.arena), "/save_golden.hex", NULL);
        size_t rn;
        uint8_t *ref = plat_path_is_file(path) ? sav_file_read(path, &rn) : NULL;
        bool same = ref && rn == hn && !memcmp(ref, hex, rn);
        if (!plat_path_is_file(path)) printf("  golden encoding skipped (no fixture: build-only tree)\n");
        else printf("  golden encoding %s (%u bytes vs %s)\n", same ? "PASS" : "FAIL", (unsigned)n, path);
        if (ref && !same) st_fail++;
        if (!ref && plat_path_is_file(path)) st_fail++;
        sav_file_free(ref);
    }
    SavFileView v;   /* the golden file also parses back */
    ST_CHECK(sav_file_view(d, n, &v) && v.kind == FOURCC('T', 'E', 'S', 'T'));
    mem_scratch_end(&s);
    sav_writer_free(w);
}

/* ================================================================== module and snapshot round trips */

/* save -> load -> save must be byte-identical for every module (§5 WP7 self-check). */
static void roundtrip_module(const SavModuleIO *m)
{
    SavWriter *a = sav_writer_new();
    m->save(a);
    size_t na;
    const uint8_t *da = sav_writer_data(a, &na);
    if (na == 0) {
        printf("  roundtrip %-7s SKIP (module writes nothing: stub)\n", m->name);
        sav_writer_free(a);
        return;
    }
    SavReader *r = sav_reader_new(da, na);
    bool lok = m->load(r) && sav_ok(r);
    size_t left = sav_remaining(r);
    sav_reader_free(r);
    SavWriter *b = sav_writer_new();
    m->save(b);
    size_t nb;
    const uint8_t *db = sav_writer_data(b, &nb);
    bool same = da && db && na == nb && memcmp(da, db, na) == 0;
    printf("  roundtrip %-7s %s (%u bytes%s%s)\n", m->name, lok && same && !left ? "PASS" : "FAIL",
           (unsigned)na, lok ? "" : ", load failed", same ? "" : ", second save differs");
    if (left) printf("             %u bytes not consumed by %s_load\n", (unsigned)left, m->name);
    if (!(lok && same && !left)) st_fail++;
    sav_writer_free(a);
    sav_writer_free(b);
}

static void selftest_modules(void)
{
    int nmodules;
    const SavModuleIO *modules = sav_modules(&nmodules);
    for (int i = 0; i < nmodules; i++) {
        if (!vm_pack() && modules[i].critical) {   /* vm/proc need a loaded Scene.pck and a started VM */
            printf("  roundtrip %-7s SKIP (no game data)\n", modules[i].name);
            continue;
        }
        roundtrip_module(&modules[i]);
    }
}

/* Synthetic non-default state so the round trips and the fuzzer see more than an empty engine. */
static void selftest_seed_state(void)
{
    Obj *o = gfx_obj(GFX_STAGE_FRONT, 3);
    if (o) {
        obj_create_rect(o, 10, 20, 100, 50, 255, 128, 0, 200);
        o->disp = true;
        ievent_set(&o->x, 300, 1000, 0, 2, false, 0, false);
        ST_CHECK(obj_children_resize(o, 2));
        if (o->nchild == 2) { o->child[1].disp = true; o->child[1].layer = 4; }
        ST_CHECK(obj_rep_resize(&o->tr_rep, &o->ntr_rep, 1));
    }
    Obj *b = gfx_obj(GFX_STAGE_BACK, 0);
    if (b) { obj_create_rect(b, 0, 0, 640, 480, 0, 0, 0, 255); b->disp = true; }
    int32_t *a = vm_bank_int(25, 17);
    if (a) *a = 12345;
    int32_t *g = vm_bank_int(31, 98);
    if (g) *g = 1;
    ws_set_utf8(&sys_state()->title, "\xe3\x83\x86\xe3\x82\xb9\xe3\x83\x88");   /* テスト */
}

static SavSnap g_st_snap;   /* the selftest's own snapshot (live state) */
static Blob g_st_slot;      /* a complete slot file made from g_st_snap */
static Blob g_st_bundle;    /* the exported bundle (fuzz corpus) */

/* Does the live state encode to exactly `snap`? */
static bool live_equals(const SavSnap *snap)
{
    SavWriter *w = sav_snap_encode(1700000000);
    size_t n;
    const uint8_t *d = sav_writer_data(w, &n);
    bool same = d && n == snap->len && !memcmp(d, snap->data, n);
    sav_writer_free(w);
    return same;
}

static void selftest_snapshot(void)
{
    if (!vm_pack()) { printf("  snapshot round trip SKIP (no game data)\n"); return; }
    SavWriter *w = sav_snap_encode(1700000000);
    ST_CHECK(sav_snap_set(&g_st_snap, w, 1700000000, &sys_state()->title));
    sav_writer_free(w);
    SavSnap keep = *sav_snap_cur();
    memset(sav_snap_cur(), 0, sizeof *sav_snap_cur());
    bool ok = sav_snap_load(g_st_snap.data, g_st_snap.len, false, true);
    bool same = live_equals(&g_st_snap);
    printf("  snapshot round trip %s (%u bytes)\n", ok && same ? "PASS" : "FAIL", (unsigned)g_st_snap.len);
    if (!(ok && same)) st_fail++;
    sav_snap_free(sav_snap_cur());
    *sav_snap_cur() = keep;
}

/* ================================================================== files */

typedef struct { Arena *ar; const char *root, *d1, *d2, *bundle; } FilesCtx;

static bool file_put(const FilesCtx *c, const char *dir, const char *name, const void *d, size_t n)
{
    return SDL_SaveFile(str_cat_arena(c->ar, dir, name, NULL), d, n);
}

static Blob file_get(const FilesCtx *c, const char *dir, const char *name)
{
    Blob b = { NULL, 0 };
    b.p = sav_file_read(str_cat_arena(c->ar, dir, name, NULL), &b.n);
    return b;
}

static void file_del(const FilesCtx *c, const char *dir, const char *name)
{
    (void)SDL_RemovePath(str_cat_arena(c->ar, dir, name, NULL));   /* may not exist */
}

/* Recursive delete of the selftest tree, depth-capped (the tree is root/<dir>/<backup dir>/<file>). */
enum { RM_DEPTH_MAX = 8 };
typedef struct { int depth; } RmCtx;
static SDL_EnumerationResult SDLCALL rm_cb(void *ud, const char *dirname, const char *fname)
{
    RmCtx *rc = ud;
    Scratch s = mem_scratch_begin(NULL);
    size_t dl = strlen(dirname);
    bool sep = dl && (dirname[dl - 1] == '/' || dirname[dl - 1] == '\\');
    const char *p = str_cat_arena(s.arena, dirname, sep ? "" : "/", fname);
    if (plat_path_is_dir(p) && rc->depth < RM_DEPTH_MAX) {
        RmCtx inner = { rc->depth + 1 };
        (void)SDL_EnumerateDirectory(p, rm_cb, &inner);   /* NOLINT(misc-no-recursion): capped by RM_DEPTH_MAX */
    }
    (void)SDL_RemovePath(p);   /* best effort: leftovers only cost disk space in the test dir */
    mem_scratch_end(&s);
    return SDL_ENUM_CONTINUE;
}

static void rm_tree(const char *dir)
{
    if (!plat_path_is_dir(dir)) return;
    RmCtx rc = { 0 };
    (void)SDL_EnumerateDirectory(dir, rm_cb, &rc);
    (void)SDL_RemovePath(dir);
}

static void files_config(void)
{
    SavConfig keep = *sav_config();
    sav_config()->audio.vol_bgm = 123; sav_config()->msg_speed = 33; sav_config()->koe_dont_stop = true;
    sav_config()->window_scale = 3;
    sav_config_write();
    memset(sav_config(), 0, sizeof *sav_config());
    ST_CHECK(sav_config_read());
    ST_CHECK(sav_config()->audio.vol_bgm == 123 && sav_config()->msg_speed == 33 && sav_config()->koe_dont_stop &&
             sav_config()->window_scale == 3 && sav_config()->audio.vol_all == keep.audio.vol_all);
    *sav_config() = keep;
}

static bool slot_info_time(int64_t t)
{
    SavInfo si;
    bool ok = sav_slot_info(7, &si) && si.unix_time == t;
    sav_info_free(&si);
    return ok;
}

/* Damaged main copies fall back to .bak / .tmp; a corrupt main is never rotated over a good .bak. */
static void files_fallbacks(const FilesCtx *c, const Blob *fa, Blob *fb, const WStr *msg)
{
    file_put(c, c->d1, "slot_007.sav", fb->p, fb->n / 2);   /* truncated main -> .bak */
    ST_CHECK(slot_info_time(1700000000));
    fb->p[fb->n / 3] ^= 4;                                    /* one flipped bit in main -> .bak */
    file_put(c, c->d1, "slot_007.sav", fb->p, fb->n);
    fb->p[fb->n / 3] ^= 4;
    ST_CHECK(slot_info_time(1700000000));
    ST_CHECK(sav_slot_write_snap(7, sav_snap_cur(), msg, NULL));
    Blob bak2 = file_get(c, c->d1, "slot_007.sav.bak");
    ST_CHECK(bak2.p && bak2.n == fa->n && !memcmp(bak2.p, fa->p, fa->n));
    blob_free(&bak2);
    file_del(c, c->d1, "slot_007.sav");                       /* main missing, complete .tmp -> .tmp */
    file_put(c, c->d1, "slot_007.sav.tmp", fb->p, fb->n);
    ST_CHECK(slot_info_time(1700000100));
    file_del(c, c->d1, "slot_007.sav.tmp");
    file_put(c, c->d1, "slot_007.sav", fb->p, fb->n);
}

/* Slot writes: the second write keeps the first as .bak; slot info; fallbacks. */
static void files_slots(const FilesCtx *c)
{
    WStr msg = { 0 };
    ws_set_utf8(&msg, "first");
    ST_CHECK(sav_slot_write_snap(7, sav_snap_cur(), &msg, NULL));
    Blob fa = file_get(c, c->d1, "slot_007.sav");
    sav_snap_cur()->time = 1700000100;
    ws_set_utf8(&msg, "second");
    uint8_t *thumb = arena_alloc_uninit(c->ar, SAV_THUMB_RGB_BYTES, 1);
    sav_thumb_capture(thumb);
    ST_CHECK(sav_slot_write_snap(7, sav_snap_cur(), &msg, thumb));
    Blob fb = file_get(c, c->d1, "slot_007.sav"), fbak = file_get(c, c->d1, "slot_007.sav.bak");
    ST_CHECK(fa.p && fb.p && fbak.p && fbak.n == fa.n && !memcmp(fbak.p, fa.p, fa.n));
    if (fb.p) {
        blob_free(&g_st_slot);
        g_st_slot = blob_dup(fb.p, fb.n);
        printf("  slot file %u bytes (snapshot %u, thumbnail %dx%d RGB565)\n", (unsigned)fb.n,
               (unsigned)sav_snap_cur()->len, SAV_THUMB_W, SAV_THUMB_H);
    }
    SavInfo si;
    ST_CHECK(sav_slot_info(7, &si) && si.used && si.unix_time == 1700000100 && sav_info_loadable(&si) && si.thumb_rgb &&
             si.thumb_w == SAV_THUMB_W && !strcmp(si.lang, lang_code()));
    sav_info_free(&si);
    if (fa.p && fb.p) files_fallbacks(c, &fa, &fb, &msg);
    blob_free(&fa); blob_free(&fb); blob_free(&fbak);
    ws_free(&msg);
}

/* global.sav with SYSG replaced by `sysg` (valid CRCs, so only the module notices) and an unknown chunk. */
static Blob global_with(const Blob *g, const uint8_t *sysg, size_t ns)
{
    SavReader *r = sav_reader_new(g->p + SAV_FILE_HDR, g->n - SAV_FILE_HDR - SAV_END_CHUNK);
    SavWriter *w = sav_writer_new();
    sav_file_begin(w, SAV_KIND_GLOB);
    uint32_t tag;
    while (sav_next_chunk(r, &tag)) {
        if (tag == SAV_TAG_SYSG) sav_put_raw_chunk(w, SAV_TAG_SYSG, sysg, ns);
        else sav_put_raw_chunk(w, tag, sav_cur(r), sav_remaining(r));
    }
    sav_put_raw_chunk(w, FOURCC('X', 'N', 'E', 'W'), (const uint8_t *)"future", 6);
    sav_file_end(w);
    sav_reader_free(r);
    return blob_take(w);
}

/* An unknown chunk (newer engine) and a chunk that fails to load survive a rewrite. */
static void global_preserve(const FilesCtx *c, const Blob *g)
{
    uint8_t sysg[13];   /* SYSG whose RFL2 chunk is cut short */
    bin_store_le32(sysg, FOURCC('R', 'F', 'L', '2')); bin_store_le32(sysg + 4, 1);
    sysg[12] = 9;
    bin_store_le32(sysg + 8, hash_crc32(0, sysg + 12, 1));
    Blob g2 = global_with(g, sysg, sizeof sysg);
    file_put(c, c->d1, "global.sav", g2.p, g2.n);
    blob_free(&g2);
    ST_CHECK(sav_global_read());          /* file valid; SYSG fails -> kept */
    ST_CHECK(sav_global_kept() == 2);
    sav_global_write();
    Blob g3 = file_get(c, c->d1, "global.sav");
    bool has_new = false, has_sys = false;
    if (g3.p) {
        SavReader *r3 = sav_reader_new(g3.p + SAV_FILE_HDR, g3.n - SAV_FILE_HDR - SAV_END_CHUNK);
        uint32_t tag;
        while (sav_next_chunk(r3, &tag)) {
            if (tag == FOURCC('X', 'N', 'E', 'W')) has_new = sav_remaining(r3) == 6;
            if (tag == SAV_TAG_SYSG)
                has_sys = sav_remaining(r3) == sizeof sysg && !memcmp(sav_cur(r3), sysg, sizeof sysg);
        }
        sav_reader_free(r3);
    }
    ST_CHECK(has_new && has_sys);
    blob_free(&g3);
}

/* A global.sav from a newer major version is neither loaded nor overwritten. */
static void global_newer(const FilesCtx *c, const Blob *g)
{
    Blob nv = blob_dup(g->p, g->n);
    bin_store_le16(nv.p + 8, SAV_VERSION + 1);
    sav_fuzz_reseal(nv.p, nv.n);
    file_del(c, c->d1, "global.sav.bak");
    file_put(c, c->d1, "global.sav", nv.p, nv.n);
    ST_CHECK(!sav_global_read() && sav_global_locked());
    sav_global_write();
    Blob nv2 = file_get(c, c->d1, "global.sav");
    ST_CHECK(nv2.p && nv2.n == nv.n && !memcmp(nv2.p, nv.p, nv.n));
    blob_free(&nv2);
    blob_free(&nv);
    sav_global_set_locked(false);
}

static void files_global(const FilesCtx *c)
{
    sav_global_write();
    Blob g = file_get(c, c->d1, "global.sav");
    ST_CHECK(g.p && sav_global_read());
    if (!g.p) return;
    global_preserve(c, &g);
    file_put(c, c->d1, "global.sav", g.p, g.n);   /* restore the good global data */
    sav_global_keep_clear();
    ST_CHECK(sav_global_read() && sav_global_kept() == 0);
    global_newer(c, &g);
    file_put(c, c->d1, "global.sav", g.p, g.n);
    ST_CHECK(sav_global_read());
    sav_global_write();                            /* corrupt main + good .bak -> .bak */
    g.p[g.n / 2] ^= 1;
    file_put(c, c->d1, "global.sav", g.p, g.n);
    ST_CHECK(sav_global_read());
    blob_free(&g);
}

/* export -> import into an empty dir gives byte-identical files; importing again backs up what it replaces; a
 * damaged bundle changes nothing. */
static void files_bundle(const FilesCtx *c)
{
    ST_CHECK(sav_export(c->bundle));
    sav_set_dir(c->d2);
    ST_CHECK(sav_import(c->bundle));
    static const char *const NAMES[] = { "slot_007.sav", "global.sav", "config.sav" };
    for (size_t i = 0; i < COUNT_OF(NAMES); i++) {
        Blob x = { NULL, 0 };
        SavFileView v;
        x.p = sav_file_read_best(str_cat_arena(c->ar, c->d1, NAMES[i], NULL), &x.n, &v, NULL);
        Blob y = file_get(c, c->d2, NAMES[i]);
        ST_CHECK(x.p && y.p && x.n == y.n && !memcmp(x.p, y.p, x.n));
        blob_free(&x); blob_free(&y);
    }
    ST_CHECK(sav_import(c->bundle));
    Blob bn = file_get(c, c->root, "all.sayosaves");
    if (bn.p) {
        blob_free(&g_st_bundle);
        g_st_bundle = blob_dup(bn.p, bn.n);
        bn.p[bn.n - 30] ^= 0x40;
        file_put(c, c->root, "bad.sayosaves", bn.p, bn.n);
        ST_CHECK(!sav_import(str_cat_arena(c->ar, c->root, "bad.sayosaves", NULL)));
        blob_free(&bn);
    }
}

static void selftest_files(void)
{
    int fail0 = st_fail;
    Scratch s = mem_scratch_begin(NULL);
    const char *saved_dir = sav_dir_peek() ? arena_strdup(s.arena, sav_dir_peek()) : NULL;
    const char *saved_src = sav_dir_source();
    FilesCtx c = { s.arena, NULL, NULL, NULL, NULL };
    c.root = sav_path_in(s.arena, "selftest_files/");
    c.d1 = str_cat_arena(s.arena, c.root, "a/", NULL);
    c.d2 = str_cat_arena(s.arena, c.root, "b/", NULL);
    c.bundle = str_cat_arena(s.arena, c.root, "all.sayosaves", NULL);
    rm_tree(c.root);
    (void)SDL_CreateDirectory(c.root);   /* failures show up as failed checks below */
    sav_set_dir(c.d1);
    files_config();
    SavSnap keep_snap = *sav_snap_cur();
    memset(sav_snap_cur(), 0, sizeof *sav_snap_cur());
    sav_snap_copy(sav_snap_cur(), &g_st_snap);
    files_slots(&c);
    files_global(&c);
    files_bundle(&c);
    printf("  files (atomic write, .bak/.tmp fallback, global preservation, export/import) %s\n",
           st_fail > fail0 ? "FAIL" : "PASS");
    sav_snap_free(sav_snap_cur());
    *sav_snap_cur() = keep_snap;
    sav_set_dir(saved_dir);
    sav_dir_set_source(saved_src);
    if (!SDL_getenv("SAYO_SAVE_KEEP_TMP")) rm_tree(c.root);
    mem_scratch_end(&s);
}

/* The engine must still restore a good snapshot after all that. */
static void fuzz_check_restore(void)
{
    if (!vm_pack()) return;
    bool ok = g_st_snap.data && sav_snap_load(g_st_snap.data, g_st_snap.len, false, true);
    bool same = ok && live_equals(&g_st_snap);
    printf("  state after fuzz restores byte-identically %s\n", same ? "PASS" : "FAIL");
    if (!same) st_fail++;
}

static void selftest_fuzz(void)
{
    st_fail += sav_fuzz_run(g_st_slot.p, g_st_slot.n, g_st_bundle.p, g_st_bundle.n);
    fuzz_check_restore();
}

/* ================================================================== format 1, language rule */

/* Format-1 slots from $SAYO_SAVE_CORPUS: they load, and convert to an identical format-2 snapshot. */
static void legacy_file(const char *path, void *ud)
{
    (void)ud;
    size_t n;
    uint8_t *d = sav_file_read(path, &n);
    SavFileView v;
    if (d && sav_file_view(d, n, &v) && v.fmt == SAV_FMT_V1 && v.kind == SAV_KIND_SLOT) {
        bool ok = sav_slot_load_bytes(0, d, n, false);
        SavWriter *w = sav_snap_encode(sav_snap_cur()->time);
        size_t n2;
        const uint8_t *d2 = sav_writer_data(w, &n2);
        bool same = ok && d2 && n2 == sav_snap_cur()->len && !memcmp(d2, sav_snap_cur()->data, n2);
        printf("  format-1 slot %s: load %s, converted snapshot %u bytes %s\n", path, ok ? "ok" : "FAILED",
               (unsigned)sav_snap_cur()->len, same ? "PASS" : "FAIL");
        if (!same) st_fail++;
        sav_writer_free(w);
    }
    sav_file_free(d);
}

static void selftest_legacy(void) { sav_fuzz_corpus_each(1 << 20, legacy_file, NULL); }

/* g_st_snap with its header rewritten for language `lang`; the innermost scene hash xor-ed with hash_xor. */
static SavSnap lang_variant(const char *lang, uint64_t hash_xor)
{
    SavSnap out = { 0 };
    SavWriter *w = sav_writer_new();
    sav_put_chunk(w, SAV_TAG_SHDR);
    sav_put_i64(w, g_st_snap.time);
    sav_put_wstr(w, &g_st_snap.title);
    sav_put_str8(w, lang);
    int sc[SAV_STACK_MAX];
    int n = vm_stack_scenes(sc, SAV_STACK_MAX);
    sav_put_u32(w, (uint32_t)n);
    for (int i = 0; i < n; i++) {
        sav_put_wstr(w, pack_scene_name(vm_pack(), sc[i]));
        sav_put_u64(w, sav_scene_hash(sc[i]) ^ (i == n - 1 ? hash_xor : 0));
    }
    (void)sav_writer_data(w, NULL);   /* close SHDR before the raw chunks */
    SavReader *r = sav_reader_new(g_st_snap.data, g_st_snap.len);
    uint32_t tag;
    while (sav_next_chunk(r, &tag))
        if (tag != SAV_TAG_SHDR) sav_put_raw_chunk(w, tag, sav_cur(r), sav_remaining(r));
    sav_reader_free(r);
    ST_CHECK(sav_snap_set(&out, w, g_st_snap.time, &g_st_snap.title));
    sav_writer_free(w);
    return out;
}

static bool slot_loadable(const SavSnap *snap, const char *want_lang, bool want_loadable, bool print_note)
{
    WStr msg = { 0 };
    Blob f = blob_take(sav_slot_encode(snap, &msg, NULL));
    SavInfo si;
    memset(&si, 0, sizeof si);
    const uint8_t *sp;
    size_t sl;
    bool legacy;
    bool ok = f.p && sav_slot_parse(f.p, f.n, &si, &sp, &sl, &legacy) && sav_info_loadable(&si) == want_loadable &&
              !strcmp(si.lang, want_lang);
    if (ok && !want_loadable) ok = strstr(si.note, "'en'") != NULL;
    if (print_note && si.note[0]) printf("  language rule: refused slot note: %s\n", si.note);
    sav_info_free(&si);
    blob_free(&f);
    return ok;
}

static void selftest_language(void)
{
    if (!vm_pack() || !g_st_snap.data) { printf("  language rule SKIP (no game data)\n"); return; }
    int fail0 = st_fail;
    ST_CHECK(sav_snap_load(g_st_snap.data, g_st_snap.len, false, true));
    SavSnap same = lang_variant("en", 0), diff = lang_variant("en", 1);
    ST_CHECK(slot_loadable(&same, "en", true, false));
    ST_CHECK(slot_loadable(&diff, "en", false, true));
    /* a refused load leaves the running state exactly as it was */
    SavWriter *before = sav_snap_encode(1700000000);
    ST_CHECK(!sav_snap_load(diff.data, diff.len, false, true) && strstr(sav_last_error(), "differs"));
    SavSnap snap_before = { 0 };
    ST_CHECK(sav_snap_set(&snap_before, before, 0, &g_st_snap.title));
    ST_CHECK(live_equals(&snap_before));
    sav_snap_free(&snap_before);
    sav_writer_free(before);
    ST_CHECK(sav_snap_load(same.data, same.len, false, true));   /* matching hashes in another language load */
    sav_snap_free(&same);
    sav_snap_free(&diff);
    ST_CHECK(sav_snap_load(g_st_snap.data, g_st_snap.len, false, true));
    printf("  language rule (other language: same scene code loads, different code refused) %s\n",
           st_fail > fail0 ? "FAIL" : "PASS");
}

/* A save made from a system screen shows the game picture frozen when the screen opened, not the screen itself. */
static void selftest_thumb_freeze(void)
{
    static uint32_t px[SCREEN_W * SCREEN_H];
    Canvas fb = { px, SCREEN_W, SCREEN_H, SCREEN_W };
    uint8_t rgb[SAV_THUMB_RGB_BYTES];
    const size_t mid = ((size_t)(SAV_THUMB_H / 2) * SAV_THUMB_W + SAV_THUMB_W / 2) * 3;
    gfx_render(&fb);                         /* fb becomes the last presented frame */
    canvas_clear(&fb, 0xffff0000u);          /* the game: red */
    sav_thumb_freeze();                      /* ui_push of the first screen */
    canvas_clear(&fb, 0xff0000ffu);          /* the save screen drawn over it: blue */
    sav_thumb_capture(rgb);
    ST_CHECK(rgb[mid] == 255 && rgb[mid + 2] == 0);
    sav_thumb_thaw();                        /* all screens closed: live picture again */
    sav_thumb_capture(rgb);
    ST_CHECK(rgb[mid] == 0 && rgb[mid + 2] == 255);
    gfx_forget_fb(&fb);
}

int sav_selftest(void)
{
    st_fail = 0;
    selftest_rw();
    printf("  writer/reader/crc/rle %s\n", st_fail ? "FAIL" : "PASS");
    selftest_golden();
    selftest_seed_state();
    selftest_modules();
    selftest_snapshot();
    int before = st_fail;
    selftest_files();
    if (st_fail > before) printf("  files FAIL (dir %s)\n", sav_dir());
    selftest_language();
    selftest_thumb_freeze();
    selftest_legacy();
    selftest_fuzz();
    sav_snap_free(&g_st_snap);
    blob_free(&g_st_slot);
    blob_free(&g_st_bundle);
    printf("  save selftest: %d failure(s)\n", st_fail);
    return st_fail ? 1 : 0;
}
