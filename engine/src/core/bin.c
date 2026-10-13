/* core/bin.c — Rd / Wr. Every read is checked against the remaining length before memory is touched; after the
 * first failure a reader stays failed and returns zeros, so callers check rd_ok() once per record. */
#include "core/bin.h"
#include "core/check.h"
#include "core/mem.h"
#include <string.h>

Rd rd_make(const void *p, size_t n)
{
    Rd r = { p, p ? n : 0, 0, p == NULL && n != 0 };
    return r;
}

void rd_fail(Rd *r) { r->err = true; }

/* Pointer to the next n bytes, advancing; NULL (and the sticky error) if they are not there. */
static const uint8_t *take(Rd *r, size_t n)
{
    if (r->err || n > r->n - r->pos) { r->err = true; return NULL; }
    const uint8_t *q = r->p + r->pos;
    r->pos += n;
    return q;
}

uint8_t rd_u8(Rd *r) { const uint8_t *q = take(r, 1); return q ? q[0] : 0; }
uint16_t rd_u16le(Rd *r) { const uint8_t *q = take(r, 2); return q ? bin_load_le16(q) : 0; }
uint32_t rd_u32le(Rd *r) { const uint8_t *q = take(r, 4); return q ? bin_load_le32(q) : 0; }
uint16_t rd_u16be(Rd *r) { const uint8_t *q = take(r, 2); return q ? bin_load_be16(q) : 0; }
uint32_t rd_u32be(Rd *r) { const uint8_t *q = take(r, 4); return q ? bin_load_be32(q) : 0; }
int32_t rd_i32le(Rd *r) { return (int32_t)rd_u32le(r); }
uint64_t rd_u64le(Rd *r) { const uint8_t *q = take(r, 8); return q ? bin_load_le64(q) : 0; }

bool rd_bytes(Rd *r, void *dst, size_t n)
{
    const uint8_t *q = take(r, n);
    if (!q) { if (dst && n) memset(dst, 0, n); return false; }
    if (n) memcpy(dst, q, n);
    return true;
}

const uint8_t *rd_view(Rd *r, size_t n) { return take(r, n); }
bool rd_skip(Rd *r, size_t n) { return take(r, n) != NULL; }

bool rd_seek(Rd *r, size_t pos)
{
    if (r->err || pos > r->n) { r->err = true; return false; }
    r->pos = pos;
    return true;
}

uint32_t rd_count(Rd *r, uint32_t max, size_t min_each)
{
    uint32_t c = rd_u32le(r);
    if (r->err || c > max || (min_each && c > rd_left(r) / min_each)) { r->err = true; return 0; }
    return c;
}

Rd rd_sub(Rd *r, size_t n)
{
    const uint8_t *q = take(r, n);
    Rd s = { q, q ? n : 0, 0, q == NULL };
    return s;
}

Rd rd_sub_at(const Rd *r, size_t off, size_t len)
{
    Rd s = { NULL, 0, 0, true };
    if (!r->err && off <= r->n && len <= r->n - off) { s.p = r->p + off; s.n = len; s.err = false; }
    return s;
}

/* ---- Wr ---- */

void wr_init_fixed(Wr *w, void *buf, size_t cap)
{
    Wr z = { buf, 0, buf ? cap : 0, buf ? cap : 0, false, false };
    *w = z;
}

void wr_init_heap(Wr *w, size_t max)
{
    Wr z = { NULL, 0, 0, max, false, true };
    *w = z;
}

void wr_free(Wr *w)
{
    if (w->heap) mem_free(w->p);
    w->p = NULL;
    w->n = w->cap = 0;
}

/* Room for n more bytes; false (sticky) when the writer is fixed and full or would exceed max. */
static bool room(Wr *w, size_t n)
{
    size_t need;
    if (w->err || !core_add_sz(w->n, n, &need) || need > w->max) { w->err = true; return false; }
    if (need <= w->cap) return true;
    if (!w->heap) { w->err = true; return false; }
    size_t cap = w->cap ? w->cap : 256;
    while (cap < need) cap = cap > w->max / 2 ? w->max : cap * 2;
    w->p = w->p ? mem_realloc(w->p, cap) : mem_alloc(cap, MEM_CORE);
    w->cap = cap;
    return true;
}

void wr_bytes(Wr *w, const void *p, size_t n)
{
    if (!n || !room(w, n)) return;
    memcpy(w->p + w->n, p, n);
    w->n += n;
}

void wr_zero(Wr *w, size_t n)
{
    if (!n || !room(w, n)) return;
    memset(w->p + w->n, 0, n);
    w->n += n;
}

void wr_u8(Wr *w, uint8_t v) { wr_bytes(w, &v, 1); }
void wr_u16le(Wr *w, uint16_t v) { uint8_t b[2]; bin_store_le16(b, v); wr_bytes(w, b, 2); }
void wr_u32le(Wr *w, uint32_t v) { uint8_t b[4]; bin_store_le32(b, v); wr_bytes(w, b, 4); }
void wr_i32le(Wr *w, int32_t v) { wr_u32le(w, (uint32_t)v); }
void wr_u64le(Wr *w, uint64_t v) { uint8_t b[8]; bin_store_le64(b, v); wr_bytes(w, b, 8); }
void wr_i64le(Wr *w, int64_t v) { wr_u64le(w, (uint64_t)v); }

bool wr_patch_u32le(Wr *w, size_t at, uint32_t v)
{
    if (w->err || at > w->n || w->n - at < 4) { w->err = true; return false; }
    bin_store_le32(w->p + at, v);
    return true;
}
