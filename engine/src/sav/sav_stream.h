/* sav/sav_stream.h — the save stream: SavWriter / SavReader, the chunked little-endian encoding every module's
 * X_save(SavWriter *) / X_load(SavReader *) uses (docs/SAVE_FORMAT.md §2). Readers are bounds-checked and sticky:
 * after a short read or bad value every further read returns 0 and sav_ok() is false. Passing a NULL writer or
 * reader is a bug (ASSERT). */
#ifndef SAV_SAV_STREAM_H
#define SAV_SAV_STREAM_H
#include "core/mem.h"
#include "core/str.h"

typedef struct SavWriter SavWriter;
typedef struct SavReader SavReader;
#define FOURCC(a, b, c, d) ((uint32_t)(uint8_t)(a) | (uint32_t)(uint8_t)(b) << 8 | \
                            (uint32_t)(uint8_t)(c) << 16 | (uint32_t)(uint8_t)(d) << 24)

/* ---- objects (tracked heap; never NULL) ---- */
SavWriter     *sav_writer_new(void);
void           sav_writer_free(SavWriter *w);
void           sav_writer_reset(SavWriter *w);   /* empty again; keeps the buffer for reuse */
/* Closes all open chunks. The bytes stay owned by w. NULL (*len = 0) when the encoding exceeded the maximum
 * save file size, so a truncated encoding is never used. */
const uint8_t *sav_writer_data(const SavWriter *w, size_t *len);
SavReader     *sav_reader_new(const uint8_t *data, size_t len);      /* data must outlive r */
void           sav_reader_free(SavReader *r);

/* ---- primitives: little-endian, fixed width (SAVE_FORMAT.md §2). A str16 is u32 count + UTF-16LE units ---- */
void     sav_put_u8(SavWriter *w, uint8_t v);
void     sav_put_i32(SavWriter *w, int32_t v);
void     sav_put_u32(SavWriter *w, uint32_t v);
void     sav_put_i64(SavWriter *w, int64_t v);
void     sav_put_u64(SavWriter *w, uint64_t v);
void     sav_put_bytes(SavWriter *w, const void *p, size_t n);
void     sav_put_wstr(SavWriter *w, const WStr *s);
void     sav_put_str8(SavWriter *w, const char *utf8);              /* u32 length + UTF-8 bytes */
uint8_t  sav_get_u8(SavReader *r);
int32_t  sav_get_i32(SavReader *r);
uint32_t sav_get_u32(SavReader *r);
int64_t  sav_get_i64(SavReader *r);
uint64_t sav_get_u64(SavReader *r);
bool     sav_get_bytes(SavReader *r, void *p, size_t n);            /* zero-fills p on failure */
bool     sav_get_wstr(SavReader *r, WStr *s);                       /* s may be NULL (skip); > WSTR_MAX_LEN fails */
bool sav_get_str8(SavReader *r, char *out, size_t cap); /* fails if longer than cap-1; out is 0-terminated */
bool     sav_ok(const SavReader *r);                                /* false after any short read / bad value */
size_t   sav_remaining(const SavReader *r);                         /* bytes left in the current chunk / level */
bool sav_has_more(const SavReader *r); /* sav_ok and bytes left (optional appended field) */
void     sav_fail(SavReader *r);                                    /* a module detected a bad value */
bool     sav_legacy(const SavReader *r);                            /* format-1 file: modules accept their old tags */
/* Reads an i32 element count; fails (returns 0) unless 0 <= n <= max and n * min_bytes_each fit in the chunk. */
int      sav_get_count(SavReader *r, int max, size_t min_bytes_each);

/* ---- chunks ----
 * A chunk is `u32 fourcc, u32 length, u32 crc32(body), body` (format 2; format-1 files had no CRC).
 * sav_put_chunk() closes the previous chunk at the same nesting level and opens a new one; sav_writer_data()
 * closes all. sav_put_enter()/sav_put_leave() nest a level inside the open chunk (sav_snap.c wraps every module's
 * X_save in its own level, so a module may use sav_put_chunk freely). sav_find_chunk() finds the first chunk with
 * that tag at the current level (any order; unknown tags and chunks with a bad CRC are skipped) and limits further
 * reads to its body; it returns false if absent (then reads fail). sav_next_chunk() iterates the chunks of the
 * current level in file order. Forward compatibility inside a chunk: readers ignore trailing bytes they do not
 * know, and new fields are only ever appended; sav_has_more() tells whether an optional trailing field is
 * present. */
void sav_put_chunk(SavWriter *w, uint32_t fourcc);
void sav_put_enter(SavWriter *w);
void sav_put_leave(SavWriter *w);
bool sav_find_chunk(SavReader *r, uint32_t fourcc);
bool sav_next_chunk(SavReader *r, uint32_t *fourcc);   /* next chunk at this level (entered); false at the end */
bool sav_get_enter(SavReader *r);
void sav_get_leave(SavReader *r);

/* ---- zero-run-length coded i32 arrays (SAVE_FORMAT.md §2.3) ----
 * sav_get_i32_rle decodes n <= max_n entries into memory from arena a (usually a scratch arena); *out is NULL on
 * failure (corrupt data, or the arena cannot hold n entries: the reader then fails). */
void sav_put_i32_rle(SavWriter *w, const int32_t *v, int n);
bool sav_get_i32_rle(SavReader *r, Arena *a, int32_t **out, int *n, int max_n);

#endif
