/* fmt/sayopak.c — SAYOPAK trailer / header / table codec (docs/PAYLOAD.md). Pure functions over bytes: no
 * allocation of its own, no SDL, so engine/tools/sayopack.c links it too. */
#include "fmt/sayopak.h"
#include <string.h>

enum { MAGIC_LEN = 8, TRAILER_MAGIC_AT = 40 };

SayopakStatus sayopak_read_trailer(const uint8_t trailer[SAYOPAK_TRAILER_SIZE], uint64_t file_size, SayopakInfo *out)
{
    memset(out, 0, sizeof *out);
    if (memcmp(trailer + TRAILER_MAGIC_AT, SAYOPAK_TRAILER_MAGIC, MAGIC_LEN) != 0) return SAYOPAK_ABSENT;
    Rd r = rd_make(trailer, SAYOPAK_TRAILER_SIZE);
    uint32_t version = rd_u32le(&r);
    SayopakInfo t;
    t.count = rd_u32le(&r);
    t.table_offset = rd_u64le(&r);
    t.table_size = rd_u64le(&r);
    t.table_crc = rd_u32le(&r);
    (void)rd_u32le(&r);                                       /* flags: 0 in version 1, ignored */
    t.payload_size = rd_u64le(&r);
    const uint64_t min_payload = SAYOPAK_HEADER_SIZE + SAYOPAK_TRAILER_SIZE;
    if (!rd_ok(&r) || version != SAYOPAK_VERSION || t.count > SAYOPAK_MAX_FILES || t.payload_size > file_size ||
        t.payload_size < min_payload || t.table_size > SAYOPAK_MAX_TABLE || t.table_offset < SAYOPAK_HEADER_SIZE ||
        t.table_offset > t.payload_size - SAYOPAK_TRAILER_SIZE ||
        t.table_size != t.payload_size - SAYOPAK_TRAILER_SIZE - t.table_offset)
        return SAYOPAK_DAMAGED;
    *out = t;
    return SAYOPAK_VALID;
}

bool sayopak_check_header(const uint8_t header[SAYOPAK_HEADER_SIZE])
{
    Rd r = rd_make(header, SAYOPAK_HEADER_SIZE);
    const uint8_t *magic = rd_view(&r, MAGIC_LEN);
    uint32_t version = rd_u32le(&r);
    return rd_ok(&r) && memcmp(magic, SAYOPAK_HEADER_MAGIC, MAGIC_LEN) == 0 && version == SAYOPAK_VERSION;
}

bool sayopak_path_safe(const char *p, size_t n)
{
    if (n == 0 || p[0] == '/' || p[n - 1] == '/') return false;
    size_t seg = 0;                                           /* bytes in the current component */
    for (size_t i = 0; i <= n; i++) {
        char c = '/';                                         /* the end closes the last component */
        if (i < n) c = p[i];
        if (c == '\\' || c == ':' || c == '\0') return false;
        if (c != '/') { seg++; continue; }
        const char *s = p + i - seg;
        if (seg == 0 || (seg == 1 && s[0] == '.') || (seg == 2 && s[0] == '.' && s[1] == '.')) return false;
        seg = 0;
    }
    return true;
}

bool sayopak_next_entry(Rd *table, const SayopakInfo *info, SayopakEntry *e)
{
    memset(e, 0, sizeof *e);
    uint64_t offset = rd_u64le(table), size = rd_u64le(table);
    uint32_t crc = rd_u32le(table);
    uint16_t path_len = rd_u16le(table);
    (void)rd_u16le(table);                                    /* flags: 0 in version 1, ignored */
    const uint8_t *path = rd_view(table, path_len);
    if (!rd_ok(table) || path_len == 0 || path_len >= SAYOPAK_MAX_PATH || offset < SAYOPAK_HEADER_SIZE ||
        offset > info->table_offset || size > info->table_offset - offset ||
        !sayopak_path_safe((const char *)path, path_len)) {
        rd_fail(table);
        return false;
    }
    e->offset = offset;
    e->size = size;
    e->crc = crc;
    e->path = (const char *)path;
    e->path_len = path_len;
    return true;
}

void sayopak_put_header(uint8_t header[SAYOPAK_HEADER_SIZE])
{
    memset(header, 0, SAYOPAK_HEADER_SIZE);
    memcpy(header, SAYOPAK_HEADER_MAGIC, MAGIC_LEN);
    bin_store_le32(header + MAGIC_LEN, SAYOPAK_VERSION);
}

void sayopak_put_entry(Wr *w, const SayopakEntry *e)
{
    if (e->path_len == 0 || e->path_len >= SAYOPAK_MAX_PATH) { w->err = true; return; }
    wr_u64le(w, e->offset);
    wr_u64le(w, e->size);
    wr_u32le(w, e->crc);
    wr_u16le(w, (uint16_t)e->path_len);
    wr_u16le(w, 0);
    wr_bytes(w, e->path, e->path_len);
}

void sayopak_put_trailer(uint8_t trailer[SAYOPAK_TRAILER_SIZE], const SayopakInfo *info)
{
    Wr w;
    wr_init_fixed(&w, trailer, SAYOPAK_TRAILER_SIZE);
    wr_u32le(&w, SAYOPAK_VERSION);
    wr_u32le(&w, info->count);
    wr_u64le(&w, info->table_offset);
    wr_u64le(&w, info->table_size);
    wr_u32le(&w, info->table_crc);
    wr_u32le(&w, 0);
    wr_u64le(&w, info->payload_size);
    wr_bytes(&w, SAYOPAK_TRAILER_MAGIC, MAGIC_LEN);
}
