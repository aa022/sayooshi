/* sav/sav_fuzz.c — save fuzzers for the selftest (--selftest save).
 *  (1) Whole files: mutated corpus files (the selftest slot, global/config encodings, the exported bundle and
 *      $SAYO_SAVE_CORPUS files), CRCs resealed 3 times in 4 so mutated payloads reach the module decoders. Each
 *      mutant is handed over in an exact-size heap block, so any over-read hits a guard / redzone in debug and
 *      sanitizer builds.
 *  (2) The stream: random byte strings driven through random SavReader operations, with the reader's bounds
 *      invariants checked after every step.
 * Environment: SAYO_SAVE_FUZZ_ITERS (default 1000), SAYO_SAVE_FUZZ_SEED, SAYO_SAVE_FUZZ_RUN (VM steps after each
 * fuzzed load), SAYO_SAVE_CORPUS. */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "vm/vm.h"
#include "vm/proc.h"
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

/* A tracked heap copy of some bytes. */
typedef struct { uint8_t *p; size_t n; } Blob;
static Blob blob_dup(const uint8_t *p, size_t n) { Blob b = { mem_dup(p, n, MEM_SAV), n }; return b; }
static void blob_free(Blob *b) { mem_free(b->p); b->p = NULL; b->n = 0; }

static Blob blob_take(SavWriter *w)   /* copy and free the writer */
{
    size_t n = 0;
    const uint8_t *d = sav_writer_data(w, &n);
    Blob b = { NULL, 0 };
    if (d) b = blob_dup(d, n);
    sav_writer_free(w);
    return b;
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void)
{
    g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17;
    return (uint32_t)(g_rng >> 16);
}

static void mutate_one(uint8_t *d, size_t *n, size_t cap)
{
    size_t at = rnd() % *n;
    switch (rnd() % 8) {
    case 0: case 1: case 2: d[at] ^= (uint8_t)(1u << (rnd() % 8)); break;
    case 3: d[at] = (uint8_t)rnd(); break;
    case 4: {
        static const uint8_t magic[] = { 0, 0xff, 0x7f, 0x80 };
        size_t m = *n - at < 4 ? *n - at : 4;
        for (size_t i = 0; i < m; i++) d[at + i] = magic[rnd() % 4];
        break;
    }
    case 5: *n = at; break;                                         /* truncate */
    case 6:                                                         /* insert random bytes */
        if (*n + 64 < cap) {
            size_t m = rnd() % 64;
            memmove(d + at + m, d + at, *n - at);
            for (size_t i = 0; i < m; i++) d[at + i] = (uint8_t)rnd();
            *n += m;
        }
        break;
    default: {                                                      /* delete a run */
        size_t m = rnd() % 64;
        if (at + m > *n) m = *n - at;
        memmove(d + at, d + at + m, *n - at - m);
        *n -= m;
        break;
    }
    }
}

static void mutate(uint8_t *d, size_t *n, size_t cap)
{
    int ops = 1 + (int)(rnd() % 4);
    for (int k = 0; k < ops && *n > 0; k++) mutate_one(d, n, cap);
}

/* Recompute every chunk CRC that still parses (and the END), so mutated payloads reach the module decoders.
 * Recursion is capped at RESEAL_DEPTH_MAX levels. */
enum { RESEAL_DEPTH_MAX = 8 };
/* NOLINTNEXTLINE(misc-no-recursion): depth capped at RESEAL_DEPTH_MAX */
static void reseal_level(uint8_t *d, size_t lo, size_t hi, int depth)
{
    size_t p = lo;
    while (hi >= SAV_HDR2 && p <= hi - SAV_HDR2) {
        uint32_t n = bin_load_le32(d + p + 4);
        if ((size_t)n > hi - p - SAV_HDR2) break;
        if (depth < RESEAL_DEPTH_MAX && n >= SAV_HDR2 &&
            sav_chunks_tile(d, p + SAV_HDR2, p + SAV_HDR2 + n, SAV_HDR2, false))
            reseal_level(d, p + SAV_HDR2, p + SAV_HDR2 + n, depth + 1);
        bin_store_le32(d + p + 8, hash_crc32(0, d + p + SAV_HDR2, n));
        p += SAV_HDR2 + n;
    }
}

void sav_fuzz_reseal(uint8_t *d, size_t n)
{
    if (n < SAV_FILE_HDR + SAV_END_CHUNK || memcmp(d, SAV_MAGIC8, 8)) return;
    size_t hi = n - SAV_END_CHUNK;
    reseal_level(d, SAV_FILE_HDR, hi, 0);
    uint8_t *e = d + hi;
    bin_store_le32(e, SAV_TAG_END); bin_store_le32(e + 4, 8);
    bin_store_le32(e + 12, (uint32_t)hi); bin_store_le32(e + 16, hash_crc32(0, d, hi));
    bin_store_le32(e + 8, hash_crc32(0, e + 12, 8));
}

typedef struct { long parsed, loaded, run, steps; } FuzzStats;   /* slot snapshots decoded / loaded, VM steps */

static void fuzz_slot(const uint8_t *d, size_t n, FuzzStats *st)
{
    SavInfo si;
    const uint8_t *snap;
    size_t sl;
    bool legacy;
    if (!sav_slot_parse(d, n, &si, &snap, &sl, &legacy)) return;
    sav_info_free(&si);
    st->parsed++;
    if (!sav_snap_load(snap, sl, legacy, true)) return;   /* state now holds whatever survived validation */
    st->loaded++;
    if (st->run > 0) {                                    /* optionally execute the restored script a little */
        proc_reset();                                     /* drop wait procs so the VM really runs */
        st->steps += vm_run_steps(st->run);
        proc_reset();
    }
    sav_writer_free(sav_snap_encode(0));                  /* ... and it must still serialise cleanly */
}

static void fuzz_one(const uint8_t *d, size_t n, FuzzStats *st)
{
    SavFileView v;
    if (!sav_file_view(d, n, &v)) return;
    if (v.kind == SAV_KIND_SLOT) {
        fuzz_slot(d, n, st);
    } else if (v.kind == SAV_KIND_GLOB) {
        (void)sav_global_decode(d, n);   /* any result is fine; it must not crash */
        sav_global_keep_clear();
    } else if (v.kind == SAV_KIND_CONF) {
        SavConfig c = *sav_config();
        (void)sav_config_decode(d, n, &c);
    } else if (v.kind == SAV_KIND_BNDL) {
        Scratch s = mem_scratch_begin(NULL);
        SavBundleFile *f = ARENA_NEW_ARRAY(s.arena, SavBundleFile, SAV_BUNDLE_MAX);
        (void)sav_bundle_parse(d, n, f, SAV_BUNDLE_MAX);
        mem_scratch_end(&s);
    }
}

/* ---- corpus: the selftest slot, the exported bundle, global/config encodings, $SAYO_SAVE_CORPUS files ---- */

enum { CORPUS_MAX = 16 };
typedef struct { Blob f[CORPUS_MAX]; int n, unusable; } Corpus;

static void corpus_add(Corpus *c, Blob b)
{
    if (b.p && c->n < CORPUS_MAX) c->f[c->n++] = b;
    else blob_free(&b);
}

void sav_fuzz_corpus_each(int max, void (*fn)(const char *path, void *ud), void *ud)
{
    const char *env = SDL_getenv("SAYO_SAVE_CORPUS");
    Str rest = str_c(env), field;
    Scratch s = mem_scratch_begin(NULL);
    for (int k = 0; k < max && rest.n && str_split(&rest, ';', &field); k++) {
        if (!field.n) continue;
        char *path = arena_alloc(s.arena, field.n + 1, 1);
        memcpy(path, field.p, field.n);
        fn(path, ud);
    }
    mem_scratch_end(&s);
}

static void corpus_env_file(const char *path, void *ud)
{
    size_t n;
    uint8_t *d = sav_file_read(path, &n);
    SavFileView v;
    if (d && sav_file_view(d, n, &v)) {
        char kind[5] = { 0 };
        bin_store_le32((uint8_t *)kind, v.kind);
        corpus_add(ud, blob_dup(d, n));
        printf("  fuzz corpus: %s (%s %.4s, %u bytes)\n", path, v.fmt == SAV_FMT_V1 ? "format 1" : "format 2", kind,
               (unsigned)n);
    } else {
        printf("  fuzz corpus: %s unusable\n", path);
        ((Corpus *)ud)->unusable++;
    }
    sav_file_free(d);
}

static void corpus_build(Corpus *c, const uint8_t *slot, size_t slot_n, const uint8_t *bundle, size_t bundle_n)
{
    memset(c, 0, sizeof *c);
    if (slot) corpus_add(c, blob_dup(slot, slot_n));
    corpus_add(c, blob_take(sav_global_encode()));
    corpus_add(c, blob_take(sav_config_encode(sav_config(), "fuzz", 0)));
    sav_fuzz_corpus_each(CORPUS_MAX, corpus_env_file, c);
    if (bundle) corpus_add(c, blob_dup(bundle, bundle_n));   /* last: keeps the seeds' order */
}

/* Whole-file mutation fuzz; returns how many mutants passed the structural checks. */
static long fuzz_files(const Corpus *c, long iters, FuzzStats *st)
{
    long accepted = 0;
    for (long i = 0; i < iters && c->n > 0; i++) {
        const Blob *src = &c->f[rnd() % (uint32_t)c->n];
        ASSERT(src->p);   /* corpus_add keeps only real files */
        size_t cap = src->n + 4096, n = src->n;
        uint8_t *m = mem_alloc(cap, MEM_SAV);
        memcpy(m, src->p, n);
        mutate(m, &n, cap);
        if (rnd() % 4 != 0) sav_fuzz_reseal(m, n);
        Blob exact = blob_dup(m, n);   /* exact size: any over-read lands in a guard / redzone */
        mem_free(m);
        SavFileView v;
        if (sav_file_view(exact.p, exact.n, &v)) accepted++;
        fuzz_one(exact.p, exact.n, st);
        blob_free(&exact);
    }
    return accepted;
}

/* ---- stream fuzz: random reader programs over random bytes, bounds invariants checked after every step ---- */

static void stream_op(SavReader *r, uint32_t op)
{
    int32_t *v = NULL;
    int n = 0;
    Str s;
    char buf[8];
    WStr ws = { 0 };
    uint32_t tag;
    switch (op % 12) {
    case 0: (void)sav_find_chunk(r, FOURCC('A' + (int)(rnd() % 3), 'A', 'A', 'A')); break;
    case 1: (void)sav_next_chunk(r, &tag); break;
    case 2: (void)sav_get_enter(r); break;
    case 3: sav_get_leave(r); break;
    case 4: (void)sav_get_u64(r); break;
    case 5: (void)sav_get_count(r, (int)(rnd() % 1000), rnd() % 16); break;
    case 6: (void)sav_get_wstr(r, &ws); break;
    case 7: (void)sav_get_str8_view(r, &s); break;
    case 8: (void)sav_get_str8(r, buf, sizeof buf); break;
    case 9: {
        Scratch sc = mem_scratch_begin(NULL);
        (void)sav_get_i32_rle(r, sc.arena, &v, &n, (int)(rnd() % 4096));
        mem_scratch_end(&sc);
        break;
    }
    case 10: sav_reset_fail(r); break;
    default: (void)sav_get_bytes(r, buf, rnd() % sizeof buf); break;
    }
    ws_free(&ws);
}

/* Bytes that look like nested chunks often enough for find/next/enter to get somewhere. */
static size_t stream_input(uint8_t *d, size_t cap)
{
    size_t n = rnd() % cap;
    for (size_t i = 0; i < n; i++) d[i] = (uint8_t)rnd();
    for (size_t p = 0; p + SAV_HDR2 <= n; p += SAV_HDR2 + (rnd() % 16)) {
        bin_store_le32(d + p, FOURCC('A' + (int)(rnd() % 3), 'A', 'A', 'A'));
        bin_store_le32(d + p + 4, rnd() % 4 ? rnd() % (uint32_t)(n - p) : rnd());
        if (rnd() % 2)
            bin_store_le32(d + p + 8,
                           hash_crc32(0, d + p + SAV_HDR2, core_min_sz(bin_load_le32(d + p + 4), n - p - SAV_HDR2)));
    }
    return n;
}

static long fuzz_stream(long iters)
{
    enum { CAP = 512, OPS = 48 };
    uint8_t in[CAP];
    long violations = 0;
    for (long i = 0; i < iters; i++) {
        size_t n = stream_input(in, CAP);
        Blob exact = blob_dup(in, n);
        SavReader *r = sav_reader_new_ex(exact.p, exact.n, rnd() % 8 == 0);
        for (int k = 0; k < OPS; k++) {
            stream_op(r, rnd());
            size_t left = sav_remaining(r);
            const uint8_t *at = sav_cur(r);
            if (left > n || (left && (at < exact.p || at + left > exact.p + n))) violations++;
            if (!sav_ok(r) && (left != 0 || sav_get_u32(r) != 0)) violations++;
        }
        sav_reader_free(r);
        blob_free(&exact);
    }
    return violations;
}

/* Environment integer in [lo, hi], else `def`. */
static long long env_int(const char *name, long long lo, long long hi, long long def)
{
    const char *s = SDL_getenv(name);
    long long v;
    return s && str_to_int_range(str_c(s), lo, hi, &v) ? v : def;
}

int sav_fuzz_run(const uint8_t *slot, size_t slot_n, const uint8_t *bundle, size_t bundle_n)
{
    long iters = (long)env_int("SAYO_SAVE_FUZZ_ITERS", 0, 100000000, 1000);
    FuzzStats st = { 0, 0, (long)env_int("SAYO_SAVE_FUZZ_RUN", 0, 100000000, 0), 0 };
    long long seed = env_int("SAYO_SAVE_FUZZ_SEED", 0, INT64_MAX, -1);
    if (seed >= 0) g_rng = (uint64_t)seed | 1;
    Corpus c;
    corpus_build(&c, slot, slot_n, bundle, bundle_n);
    log_set_level(-1);   /* thousands of expected "did not load" warnings */
    long accepted = fuzz_files(&c, iters, &st);
    long stream_iters = iters / 2 + 100, violations = fuzz_stream(stream_iters);
    log_set_level(LOG_INFO);
    for (int i = 0; i < c.n; i++) blob_free(&c.f[i]);
    printf("  fuzz: %ld mutated files, %ld passed the structural checks, %ld slot snapshots decoded (%ld loaded), "
           "no crash\n", iters, accepted, st.parsed, st.loaded);
    if (st.run > 0) printf("  fuzz: %ld VM instructions executed after the loads\n", st.steps);
    printf("  fuzz: %ld random reader programs, %ld bounds violations %s\n", stream_iters, violations,
           violations ? "FAIL" : "PASS");
    return c.unusable + (violations ? 1 : 0);
}
