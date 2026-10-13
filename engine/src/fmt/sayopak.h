/* fmt/sayopak.h — SAYOPAK archive format: constants and the codec shared by the engine (plat/fs.c) and the host
 * pack tool (engine/tools/sayopack.c). Full description: docs/PAYLOAD.md. All integers are little-endian.
 *
 *   payload := header  file-data...  table  trailer
 *   header  (16 bytes, at payload start)   "SAYOPAKH" u32 version u32 reserved(0)
 *   data    each file stored uncompressed, starting at a 16-byte aligned payload offset, zero padding between
 *   table   per entry: u64 offset  u64 size  u32 crc32  u16 path_len  u16 flags(0)  path[path_len] (UTF-8, '/')
 *   trailer (48 bytes, the last bytes of the file)
 *           u32 version  u32 count  u64 table_offset  u64 table_size  u32 table_crc32  u32 flags(0)
 *           u64 payload_size (header .. trailer inclusive)  "SAYOPAK1"
 * Offsets are relative to the payload start = file_size - payload_size, so the same bytes work as a
 * stand-alone game.pak and appended to an executable.
 *
 * The codec is pure (bytes in, validated values out) and depends only on core/bin.h, so the SDL-free host tool can
 * link it. Every value read from a file is validated here, once, for both users. */
#ifndef FMT_SAYOPAK_H
#define FMT_SAYOPAK_H
#include "core/bin.h"

#define SAYOPAK_HEADER_MAGIC  "SAYOPAKH"
#define SAYOPAK_TRAILER_MAGIC "SAYOPAK1"
#define SAYOPAK_VERSION       1u
#define SAYOPAK_HEADER_SIZE   16
#define SAYOPAK_TRAILER_SIZE  48
#define SAYOPAK_ENTRY_FIXED   24      /* bytes per table entry before the path */
#define SAYOPAK_ALIGN         16
#define SAYOPAK_MAX_PATH      1024    /* path_len < SAYOPAK_MAX_PATH */
#define SAYOPAK_MAX_FILES     1000000u
#define SAYOPAK_MAX_TABLE     ((uint64_t)256 << 20)

typedef enum SayopakStatus { SAYOPAK_ABSENT, SAYOPAK_VALID, SAYOPAK_DAMAGED } SayopakStatus;

/* The validated trailer. Offsets are relative to the payload start (base = file size - payload_size). */
typedef struct SayopakInfo {
    uint32_t count, table_crc;
    uint64_t table_offset, table_size, payload_size;
} SayopakInfo;

/* One validated table entry. path points into the table bytes (not NUL-terminated) and is a safe relative path. */
typedef struct SayopakEntry {
    uint64_t offset, size;
    uint32_t crc;
    const char *path;
    size_t path_len;
} SayopakEntry;

/* trailer = the last SAYOPAK_TRAILER_SIZE bytes of a file of file_size bytes. ABSENT: no trailer magic (not an
 * archive); DAMAGED: the magic is there but a field is impossible; VALID: *out filled. */
SayopakStatus sayopak_read_trailer(const uint8_t trailer[SAYOPAK_TRAILER_SIZE], uint64_t file_size, SayopakInfo *out);
/* header = the first SAYOPAK_HEADER_SIZE bytes of the payload. */
bool sayopak_check_header(const uint8_t header[SAYOPAK_HEADER_SIZE]);
/* Reads the next entry of the table (a reader over exactly info->table_size bytes). false (and the reader failed)
 * on a truncated entry, an unsafe path, or a data range outside [header, table). */
bool sayopak_next_entry(Rd *table, const SayopakInfo *info, SayopakEntry *e);
/* Archive paths: relative, '/' separated, no empty, "." or ".." components, no '\\', ':' or NUL. */
bool sayopak_path_safe(const char *p, size_t n);

/* Writers (host tool). Entries are appended to w; header/trailer fill fixed arrays. */
void sayopak_put_header(uint8_t header[SAYOPAK_HEADER_SIZE]);
void sayopak_put_entry(Wr *w, const SayopakEntry *e);
void sayopak_put_trailer(uint8_t trailer[SAYOPAK_TRAILER_SIZE], const SayopakInfo *info);

#endif
