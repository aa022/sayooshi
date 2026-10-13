/* core/bin.h — little-endian binary data: raw load/store helpers, and the bounds-checked reader (Rd) and
 * writer (Wr) that every format parser and the save code use (docs/CODE_STANDARD.md §5).
 *
 * Rd never reads outside [p, p + n): every read past the end returns 0 (or zero-fills) and sets a sticky
 * error flag, so a parser can read a whole record and check rd_ok() once at the end. Wr appends to a buffer
 * that grows on the tracked heap (or writes into fixed caller storage); out of room is likewise sticky. */
#ifndef CORE_BIN_H
#define CORE_BIN_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- raw helpers: the caller guarantees p has room (use Rd/Wr for anything external) ---- */
static inline uint16_t bin_load_le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t bin_load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t bin_load_le64(const uint8_t *p)
{
    return (uint64_t)bin_load_le32(p) | (uint64_t)bin_load_le32(p + 4) << 32;
}
static inline uint16_t bin_load_be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline uint32_t bin_load_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static inline void bin_store_le16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void bin_store_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}
static inline void bin_store_le64(uint8_t *p, uint64_t v)
{
    bin_store_le32(p, (uint32_t)v);
    bin_store_le32(p + 4, (uint32_t)(v >> 32));
}

/* ---- Rd: bounds-checked little-endian reader with a sticky error ------------------------------------------- */
typedef struct Rd { const uint8_t *p; size_t n, pos; bool err; } Rd;
Rd       rd_make(const void *p, size_t n);          /* p may be NULL when n == 0 */
static inline bool   rd_ok(const Rd *r) { return !r->err; }
static inline size_t rd_left(const Rd *r) { return r->err ? 0 : r->n - r->pos; }
void     rd_fail(Rd *r);                            /* mark invalid data found by the caller */
uint8_t  rd_u8(Rd *r);
uint16_t rd_u16le(Rd *r);
uint32_t rd_u32le(Rd *r);
uint16_t rd_u16be(Rd *r);                          /* big-endian (TrueType tables) */
uint32_t rd_u32be(Rd *r);
int32_t  rd_i32le(Rd *r);
uint64_t rd_u64le(Rd *r);
bool     rd_bytes(Rd *r, void *dst, size_t n);      /* zero-fills dst on failure */
const uint8_t *rd_view(Rd *r, size_t n);            /* n bytes in place (advances); NULL on failure */
bool     rd_skip(Rd *r, size_t n);
bool     rd_seek(Rd *r, size_t pos);                /* absolute; pos == n is allowed */
/* Element count that must be <= max and leave room for count * min_each bytes; 0 + error otherwise. */
uint32_t rd_count(Rd *r, uint32_t max, size_t min_each);
/* Reader over the next n bytes (advances r). Failure leaves r failed and returns a failed reader. */
Rd       rd_sub(Rd *r, size_t n);
/* Reader over [off, off + len) of r's whole buffer (offset tables); a failed reader if out of range. */
Rd       rd_sub_at(const Rd *r, size_t off, size_t len);

/* ---- Wr: little-endian writer into fixed storage or a growing tracked-heap buffer, sticky error ------------- */
typedef struct Wr { uint8_t *p; size_t n, cap, max; bool err, heap; } Wr;
void     wr_init_fixed(Wr *w, void *buf, size_t cap);
void     wr_init_heap(Wr *w, size_t max);           /* grows on demand up to max bytes */
void     wr_free(Wr *w);                            /* heap writers only; fixed storage is the caller's */
static inline bool           wr_ok(const Wr *w) { return !w->err; }
static inline size_t         wr_len(const Wr *w) { return w->n; }
static inline const uint8_t *wr_data(const Wr *w) { return w->p; }
void     wr_u8(Wr *w, uint8_t v);
void     wr_u16le(Wr *w, uint16_t v);
void     wr_u32le(Wr *w, uint32_t v);
void     wr_i32le(Wr *w, int32_t v);
void     wr_u64le(Wr *w, uint64_t v);
void     wr_i64le(Wr *w, int64_t v);
void     wr_bytes(Wr *w, const void *p, size_t n);
void     wr_zero(Wr *w, size_t n);
bool     wr_patch_u32le(Wr *w, size_t at, uint32_t v);   /* overwrite 4 already written bytes (length fields) */

#endif
