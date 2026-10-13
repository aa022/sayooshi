/* sav/sav_stream.c — SavWriter / SavReader: the chunked little-endian stream of save format 2
 * (docs/SAVE_FORMAT.md §2): `u32 fourcc, u32 length, u32 crc32(body), body`, nested levels, RLE int arrays.
 * Format-1 (legacy) readers use 8-byte chunk headers without CRC.
 *
 * Memory: a writer appends to a core Wr (tracked heap, capped at SAV_MAX_FILE_BYTES: a larger file could never be
 * read back, so the writer fails instead); writer and reader objects are tracked heap blocks owned by the caller.
 * Failure is sticky on both sides: a failed writer hands out no data at all, a failed reader returns zeros. */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/check.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include <string.h>

struct SavWriter {
    Wr   out;
    int  depth;                             /* current nesting level (0 = top) */
    bool open[SAV_SEC_MAX_DEPTH];           /* a chunk is open at that level ... */
    size_t at[SAV_SEC_MAX_DEPTH];           /* ... and its header starts here */
};

typedef struct SavLevel {
    size_t lo, hi;                          /* the level's body [lo, hi) */
    bool   entered;                         /* a chunk of this level was entered (find/next) ... */
    size_t end;                             /* ... and its body ends here */
    size_t it;                              /* sav_next_chunk cursor */
} SavLevel;

struct SavReader {
    const uint8_t *d;
    size_t pos;
    bool   fail, legacy;
    int    depth;
    SavLevel lv[SAV_SEC_MAX_DEPTH];
};

/* ================================================================== writer */

SavWriter *sav_writer_new(void)
{
    SavWriter *w = mem_zalloc(sizeof *w, MEM_SAV);
    wr_init_heap(&w->out, SAV_MAX_FILE_BYTES);
    return w;
}

/* Empties w for a new stream and keeps its buffer, so a writer reused per snapshot stops reallocating. */
void sav_writer_reset(SavWriter *w)
{
    ASSERT(w && w->out.heap);
    Wr out = w->out;
    out.n = 0;
    out.err = false;
    memset(w, 0, sizeof *w);
    w->out = out;
}

void sav_writer_free(SavWriter *w)
{
    if (!w) return;
    wr_free(&w->out);
    mem_free(w);
}


void sav_put_bytes(SavWriter *w, const void *p, size_t n)
{
    ASSERT(w && (p || n == 0));
    wr_bytes(&w->out, p, n);
}

void sav_put_u8(SavWriter *w, uint8_t v) { ASSERT(w); wr_u8(&w->out, v); }
void sav_put_u32(SavWriter *w, uint32_t v) { ASSERT(w); wr_u32le(&w->out, v); }
void sav_put_i32(SavWriter *w, int32_t v) { ASSERT(w); wr_i32le(&w->out, v); }
void sav_put_u64(SavWriter *w, uint64_t v) { ASSERT(w); wr_u64le(&w->out, v); }
void sav_put_i64(SavWriter *w, int64_t v) { ASSERT(w); wr_i64le(&w->out, v); }

void sav_put_wstr(SavWriter *w, const WStr *s)
{
    int n = s && s->p && s->len > 0 ? s->len : 0;
    sav_put_u32(w, (uint32_t)n);
    for (int i = 0; i < n; i++) wr_u16le(&w->out, s->p[i]);
}

void sav_put_str8(SavWriter *w, const char *utf8)
{
    size_t n = utf8 ? strlen(utf8) : 0;
    ASSERT_MSG(n <= UINT32_MAX, "save: string too long");
    sav_put_u32(w, (uint32_t)n);
    sav_put_bytes(w, utf8, n);
}

/* Patch length and CRC of the chunk open at `lvl`. */
static void close_level(SavWriter *w, int lvl)
{
    if (!w->open[lvl]) return;
    w->open[lvl] = false;
    if (!wr_ok(&w->out)) return;
    size_t h = w->at[lvl], body = h + SAV_HDR2, n = wr_len(&w->out) - body;
    (void)wr_patch_u32le(&w->out, h + 4, (uint32_t)n);   /* in range by construction; failure would be sticky */
    (void)wr_patch_u32le(&w->out, h + 8, hash_crc32(0, wr_data(&w->out) + body, n));
}

void sav_put_chunk(SavWriter *w, uint32_t fourcc)
{
    ASSERT(w);
    close_level(w, w->depth);
    w->open[w->depth] = true;
    w->at[w->depth] = wr_len(&w->out);
    wr_u32le(&w->out, fourcc);
    wr_u32le(&w->out, 0);   /* length, patched on close */
    wr_u32le(&w->out, 0);   /* crc32 of the body, patched on close */
}

void sav_put_enter(SavWriter *w)
{
    ASSERT(w);
    ASSERT_MSG(w->depth + 1 < SAV_SEC_MAX_DEPTH, "save: chunk nesting deeper than %d", SAV_SEC_MAX_DEPTH);
    w->depth++;
    w->open[w->depth] = false;
}

void sav_put_leave(SavWriter *w)
{
    ASSERT(w);
    ASSERT_MSG(w->depth > 0, "save: sav_put_leave without sav_put_enter");
    close_level(w, w->depth);
    w->depth--;
}

/* Closes every open chunk (only patches headers, hence the const in the public signature). NULL + 0 when the
 * writer failed (over SAV_MAX_FILE_BYTES): a truncated encoding must never reach a file. */
const uint8_t *sav_writer_data(const SavWriter *cw, size_t *len)
{
    SavWriter *w = (SavWriter *)cw;
    ASSERT(w);
    for (int l = w->depth; l >= 0; l--) close_level(w, l);
    bool ok = wr_ok(&w->out);
    if (len) *len = ok ? wr_len(&w->out) : 0;
    return ok ? wr_data(&w->out) : NULL;
}

/* Chunk with a known body (END, preserved chunks). The open chunk of this level is closed first. */
void sav_put_raw_chunk(SavWriter *w, uint32_t tag, const uint8_t *body, size_t n)
{
    ASSERT(w && (body || n == 0) && n <= UINT32_MAX);
    close_level(w, w->depth);
    wr_u32le(&w->out, tag);
    wr_u32le(&w->out, (uint32_t)n);
    wr_u32le(&w->out, hash_crc32(0, body, n));
    wr_bytes(&w->out, body, n);
}

/* ================================================================== reader */

SavReader *sav_reader_new_ex(const uint8_t *data, size_t len, bool legacy)
{
    SavReader *r = mem_zalloc(sizeof *r, MEM_SAV);
    r->d = data;
    r->lv[0].hi = data ? len : 0;
    r->legacy = legacy;
    return r;
}

SavReader *sav_reader_new(const uint8_t *data, size_t len) { return sav_reader_new_ex(data, len, false); }
void sav_reader_free(SavReader *r) { mem_free(r); }
bool sav_ok(const SavReader *r) { return r && !r->fail; }
void sav_fail(SavReader *r) { if (r) r->fail = true; }
bool sav_legacy(const SavReader *r) { return r && r->legacy; }
void sav_reset_fail(SavReader *r) { ASSERT(r); r->fail = false; }

static size_t hdr_size(const SavReader *r) { return r->legacy ? SAV_HDR1 : SAV_HDR2; }

static size_t read_limit(const SavReader *r)
{
    const SavLevel *l = &r->lv[r->depth];
    return l->entered ? l->end : l->hi;
}

size_t sav_remaining(const SavReader *r)
{
    if (!r || r->fail) return 0;
    size_t lim = read_limit(r);
    return r->pos < lim ? lim - r->pos : 0;
}

bool sav_has_more(const SavReader *r) { return sav_remaining(r) > 0; }

const uint8_t *sav_cur(const SavReader *r)
{
    ASSERT(r);
    return r->d ? r->d + r->pos : NULL;
}

/* n bytes in place (advancing), or NULL and a failed reader. */
static const uint8_t *take(SavReader *r, size_t n)
{
    ASSERT(r);
    if (r->fail || sav_remaining(r) < n) { r->fail = true; return NULL; }
    const uint8_t *p = r->d + r->pos;
    r->pos += n;
    return p;
}

bool sav_get_bytes(SavReader *r, void *p, size_t n)
{
    const uint8_t *s = take(r, n);
    if (p && n) {
        if (s) memcpy(p, s, n);
        else memset(p, 0, n);
    }
    return s != NULL;
}

uint8_t sav_get_u8(SavReader *r) { const uint8_t *p = take(r, 1); return p ? p[0] : 0; }
uint32_t sav_get_u32(SavReader *r) { const uint8_t *p = take(r, 4); return p ? bin_load_le32(p) : 0; }
int32_t sav_get_i32(SavReader *r) { return (int32_t)sav_get_u32(r); }
uint64_t sav_get_u64(SavReader *r) { const uint8_t *p = take(r, 8); return p ? bin_load_le64(p) : 0; }
int64_t sav_get_i64(SavReader *r) { return (int64_t)sav_get_u64(r); }

int sav_get_count(SavReader *r, int max, size_t min_bytes_each)
{
    int32_t n = sav_get_i32(r);
    if (!sav_ok(r)) return 0;
    if (n < 0 || n > max || (min_bytes_each && (size_t)n > sav_remaining(r) / min_bytes_each)) {
        r->fail = true;
        return 0;
    }
    return n;
}

bool sav_get_wstr(SavReader *r, WStr *s)
{
    uint32_t n = sav_get_u32(r);
    const uint8_t *p = sav_ok(r) && n <= WSTR_MAX_LEN ? take(r, (size_t)n * 2) : NULL;
    if (!p) r->fail = true;
    if (!s) return p != NULL;
    ws_set(s, NULL, 0);
    for (uint32_t i = 0; p && i < n; i++) ws_push(s, bin_load_le16(p + (size_t)i * 2));
    return p != NULL;
}

bool sav_get_str8_view(SavReader *r, Str *out)
{
    uint32_t n = sav_get_u32(r);
    const uint8_t *p = sav_ok(r) ? take(r, n) : NULL;
    *out = p ? str_make((const char *)p, n) : str_make("", 0);
    return p != NULL;
}

bool sav_get_str8(SavReader *r, char *out, size_t cap)
{
    ASSERT(out && cap > 0);
    Str s;
    if (!sav_get_str8_view(r, &s) || s.n >= cap) {
        r->fail = true;
        out[0] = 0;
        return false;
    }
    bool fit = str_copy(out, cap, s);
    ASSERT(fit);   /* length checked above */
    return true;
}

/* ================================================================== chunks */

/* Header of the chunk at p inside [.., hi): tag and body length; false when it does not fit. */
static bool chunk_at(const SavReader *r, size_t p, size_t hi, uint32_t *tag, uint32_t *n)
{
    size_t h = hdr_size(r);
    if (hi < h || p > hi - h) return false;
    *tag = bin_load_le32(r->d + p);
    *n = bin_load_le32(r->d + p + 4);
    return (size_t)*n <= hi - p - h;   /* a corrupt length: nothing after it can be trusted */
}

static bool chunk_crc_ok(const SavReader *r, size_t p, uint32_t n)
{
    if (r->legacy) return true;
    return bin_load_le32(r->d + p + 8) == hash_crc32(0, r->d + p + SAV_HDR2, n);
}

static void enter_body(SavReader *r, size_t p, uint32_t n)
{
    SavLevel *l = &r->lv[r->depth];
    r->pos = p + hdr_size(r);
    l->entered = true;
    l->end = r->pos + n;
}

/* Absent chunk: stray reads fail instead of returning bytes of another chunk. */
static void park(SavReader *r)
{
    SavLevel *l = &r->lv[r->depth];
    r->pos = l->hi;
    l->entered = true;
    l->end = l->hi;
}

static void warn_bad_crc(const SavReader *r, size_t p, const char *what)
{
    char tag[5] = { 0 };
    memcpy(tag, r->d + p, 4);
    log_msg(LOG_WARN, "save: chunk %.4s has a bad CRC; %s", tag, what);
}

/* Each step advances by at least one header, so both scans are bounded by the level size. */
bool sav_find_chunk(SavReader *r, uint32_t fourcc)
{
    if (!r || r->fail) return false;
    const SavLevel *l = &r->lv[r->depth];
    uint32_t tag, n;
    for (size_t p = l->lo; chunk_at(r, p, l->hi, &tag, &n); p += hdr_size(r) + (size_t)n) {
        if (tag != fourcc) continue;
        if (chunk_crc_ok(r, p, n)) { enter_body(r, p, n); return true; }
        warn_bad_crc(r, p, "ignored");
    }
    park(r);
    return false;
}

bool sav_next_chunk(SavReader *r, uint32_t *fourcc)
{
    if (!r || r->fail) return false;
    SavLevel *l = &r->lv[r->depth];
    uint32_t tag, n;
    size_t p = l->it < l->lo ? l->lo : l->it;
    while (chunk_at(r, p, l->hi, &tag, &n)) {
        l->it = p + hdr_size(r) + (size_t)n;
        if (chunk_crc_ok(r, p, n)) {
            enter_body(r, p, n);
            if (fourcc) *fourcc = tag;
            return true;
        }
        warn_bad_crc(r, p, "skipped");
        p = l->it;
    }
    l->it = l->hi;
    park(r);
    return false;
}

bool sav_get_enter(SavReader *r)
{
    if (!r || r->fail) return false;
    const SavLevel *l = &r->lv[r->depth];
    if (!l->entered || r->depth + 1 >= SAV_SEC_MAX_DEPTH) { r->fail = true; return false; }
    SavLevel *in = &r->lv[++r->depth];
    in->lo = in->it = r->pos;
    in->hi = l->end;
    in->entered = false;
    return true;
}

void sav_get_leave(SavReader *r)
{
    if (!r || r->depth == 0) return;
    r->depth--;
    r->pos = read_limit(r);
}

/* ================================================================== zero-run-length i32 arrays (§2.3) */

void sav_put_i32_rle(SavWriter *w, const int32_t *v, int n)
{
    if (n < 0 || !v) n = 0;
    sav_put_u32(w, (uint32_t)n);
    int i = 0;
    while (i < n) {
        int z = 0;
        while (i + z < n && v[i + z] == 0) z++;
        i += z;
        int s = i;
        while (i < n && !(v[i] == 0 && (i + 1 >= n || v[i + 1] == 0))) i++;   /* literals end at two zeros */
        sav_put_u32(w, (uint32_t)z);
        sav_put_u32(w, (uint32_t)(i - s));
        for (int k = s; k < i; k++) sav_put_i32(w, v[k]);
    }
}

/* Segments into v[0..n) (zeroed by the caller). Every segment consumes 8+ bytes, so the loop is bounded by the
 * chunk; an empty segment or one that overruns n is corrupt data. */
static bool rle_segments(SavReader *r, int32_t *v, int n)
{
    int filled = 0;
    while (filled < n && sav_ok(r)) {
        uint32_t z = sav_get_u32(r), l = sav_get_u32(r);
        if (!sav_ok(r) || (z == 0 && l == 0) || z > (uint32_t)(n - filled)) { sav_fail(r); break; }
        filled += (int)z;
        if (l > (uint32_t)(n - filled) || l > sav_remaining(r) / 4) { sav_fail(r); break; }
        for (uint32_t k = 0; k < l; k++) v[filled++] = sav_get_i32(r);
    }
    return sav_ok(r);
}

bool sav_get_i32_rle(SavReader *r, Arena *a, int32_t **out, int *n_out, int max_n)
{
    *out = NULL;
    *n_out = 0;
    int n = sav_get_count(r, max_n, 0);
    if (!sav_ok(r)) return false;
    int32_t *v = arena_try_alloc(a, (size_t)(n ? n : 1) * sizeof *v, _Alignof(int32_t));   /* zeroed */
    if (!v) { sav_fail(r); return false; }
    if (!rle_segments(r, v, n)) return false;   /* v stays in the arena until the caller's reset */
    *out = v;
    *n_out = n;
    return true;
}
