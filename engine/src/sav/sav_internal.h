/* sav/sav_internal.h — shared by the sav/ sources only (never included outside sav/): file framing constants,
 * the file layer (sav_file.c), the snapshot store (sav_snap.c), the slot/global/config/bundle codecs the selftest
 * drives, and module-internal access to the stream.
 *
 * Ownership in this module: encoders return a SavWriter that owns the encoded file (sav_writer_free); file reads
 * return tracked-heap buffers (sav_file_free); paths are built in an arena the caller passes (normally a
 * mem_scratch_begin() scratch arena released when the caller returns). */
#ifndef SAV_SAV_INTERNAL_H
#define SAV_SAV_INTERNAL_H
#include "core/mem.h"
#include "core/str.h"
#include "sav/sav.h"
#include "sav/sav_stream.h"

/* ---- framing (docs/SAVE_FORMAT.md §3) ---- */
enum { SAV_SEC_MAX_DEPTH = 16,           /* chunk nesting levels */
       SAV_HDR2 = 12, SAV_HDR1 = 8,      /* chunk header: format 2 (tag, length, crc) / format 1 (tag, length) */
       SAV_FILE_HDR = 16,                /* "SAYOSAVE", u16 major, u16 minor, u32 kind */
       SAV_END_CHUNK = 20,               /* END chunk: header + u32 offset + u32 whole-file CRC */
       SAV_MAX_FILE_BYTES = 64 << 20,    /* larger files are neither written nor read */
       SAV_STACK_MAX = 64,               /* call-stack scenes recorded per snapshot */
       SAV_NAME_CAP = 16,                /* "slot_099.sav", "quick_2.sav", "global.sav" + NUL */
       SAV_ERR_CAP = 256 };              /* sav_last_error() text (longer messages are cut, never overflow) */
#define SAV_MAGIC8     "SAYOSAVE"
#define SAV_GAME_ID    "sayooshi"
#define SAV_OLD_MAGIC  FOURCC('S', 'A', 'Y', 'O')   /* format 1 */
#define SAV_KIND_SLOT  FOURCC('S', 'L', 'O', 'T')
#define SAV_KIND_GLOB  FOURCC('G', 'L', 'O', 'B')
#define SAV_KIND_CONF  FOURCC('C', 'O', 'N', 'F')
#define SAV_KIND_BNDL  FOURCC('B', 'N', 'D', 'L')
#define SAV_TAG_END    FOURCC('E', 'N', 'D', ' ')
#define SAV_TAG_META   FOURCC('M', 'E', 'T', 'A')
#define SAV_TAG_SHDR   FOURCC('S', 'H', 'D', 'R')
#define SAV_TAG_STAK   FOURCC('S', 'T', 'A', 'K')
#define SAV_TAG_HEAD   FOURCC('H', 'E', 'A', 'D')
#define SAV_TAG_INFO   FOURCC('I', 'N', 'F', 'O')
#define SAV_TAG_SNAP   FOURCC('S', 'N', 'A', 'P')
#define SAV_TAG_THMB   FOURCC('T', 'H', 'M', 'B')
#define SAV_TAG_TITL   FOURCC('T', 'I', 'T', 'L')
#define SAV_TAG_MESG   FOURCC('M', 'E', 'S', 'G')
#define SAV_TAG_GAME   FOURCC('G', 'A', 'M', 'E')
#define SAV_TAG_ENGV   FOURCC('E', 'N', 'G', 'V')
#define SAV_TAG_LANG   FOURCC('L', 'A', 'N', 'G')
#define SAV_TAG_TIME   FOURCC('T', 'I', 'M', 'E')
#define SAV_TAG_PACK   FOURCC('P', 'A', 'C', 'K')
#define SAV_TAG_VMGL   FOURCC('V', 'M', 'G', 'L')
#define SAV_TAG_SYSG   FOURCC('S', 'Y', 'S', 'G')
#define SAV_TAG_CGTG   FOURCC('C', 'G', 'T', 'G')
#define SAV_TAG_CFG2   FOURCC('C', 'F', 'G', '2')
#define SAV_TAG_FILE   FOURCC('F', 'I', 'L', 'E')
#define SAV_TAG_NAME   FOURCC('N', 'A', 'M', 'E')
#define SAV_TAG_DATA   FOURCC('D', 'A', 'T', 'A')

/* ---- stream internals (sav_stream.c) ---- */
SavReader     *sav_reader_new_ex(const uint8_t *data, size_t len, bool legacy);
const uint8_t *sav_cur(const SavReader *r);                 /* data at the read position (sav_remaining() bytes) */
void           sav_reset_fail(SavReader *r);                /* clear the sticky error (skip a damaged optional part) */
bool           sav_get_str8_view(SavReader *r, Str *out);   /* str8 in place (valid while the data lives) */
void           sav_put_raw_chunk(SavWriter *w, uint32_t tag, const uint8_t *body, size_t n);

/* ---- errors, directory, paths (sav_file.c) ---- */
void        sav_set_error(const char *fmt, ...) CORE_PRINTF(1, 2);   /* sav_last_error() text */
void        sav_clear_error(void);
/* Display text (notes) into a fixed field: cut at the field size, never overflowing (a cut note is still a note). */
int64_t     sav_now_unix(void);
const char *sav_dir_peek(void);                              /* NULL until resolved */
const char *sav_dir_source(void);
void        sav_dir_set_source(const char *source);          /* a string literal */
void        sav_dir_free(void);
const char *sav_path_in(Arena *ar, const char *name);       /* save dir + name */
/* File name of a slot (format 2, or the format-1 name when legacy); false for a slot out of range. */
bool        sav_slot_name(int slot, char out[SAV_NAME_CAP], bool legacy);

/* ---- file framing and atomic I/O (sav_file.c) ---- */
typedef enum { SAV_FMT_BAD = 0, SAV_FMT_V2, SAV_FMT_V1 } SavFileFmt;
typedef struct { SavFileFmt fmt; uint32_t kind; size_t lo, hi; uint16_t major, minor; } SavFileView;
void        sav_file_begin(SavWriter *w, uint32_t kind);
void        sav_file_end(SavWriter *w);                      /* closes all chunks and appends END */
bool        sav_chunks_tile(const uint8_t *d, size_t lo, size_t hi, size_t h, bool crc);
bool        sav_file_view(const uint8_t *d, size_t len, SavFileView *v);
SavReader  *sav_view_reader(const uint8_t *d, const SavFileView *v);
bool        sav_file_seen_newer(void);                       /* sav_file_view met a newer major version */
void        sav_file_clear_newer(void);
uint8_t    *sav_file_read(const char *path, size_t *len);   /* NULL if missing/too large; release: sav_file_free */
void        sav_file_free(uint8_t *data);
bool        sav_file_write(const char *path, const uint8_t *data, size_t len, uint32_t kind);
bool        sav_file_store(const char *path, const SavWriter *w, uint32_t kind);   /* NULL/failed w -> false */
/* Best valid version of a file: main, else .tmp, else .bak (*which 0/1/2). Release with sav_file_free. */
uint8_t    *sav_file_read_best(const char *path, size_t *len, SavFileView *view, int *which);
bool        sav_file_exists_any(const char *path);
void        sav_file_quarantine(const char *path);
void        sav_file_copy_once(const char *path, const char *suffix);
void        sav_put_meta(SavWriter *w, const char *lang, int64_t t);

/* ---- snapshots (sav_snap.c) ---- */
typedef struct SavSnap { uint8_t *data; size_t len; int64_t time; WStr title; } SavSnap;   /* data: tracked heap */
typedef struct {
    int64_t time;
    WStr    title;
    char    lang[SAV_LANG_CAP];
    bool    stack_ok;
    char    why[SAV_NOTE_CAP];
} SavSnapHead;
typedef void (*SavSaveFn)(SavWriter *);
typedef bool (*SavLoadFn)(SavReader *);
typedef struct { uint32_t tag; const char *name; SavSaveFn save; SavLoadFn load; bool critical; } SavModuleIO;
SavSnap           *sav_snap_cur(void);              /* the latest msg_block / savepoint snapshot */
void               sav_snap_free(SavSnap *s);
void               sav_snap_copy(SavSnap *dst, const SavSnap *src);
bool               sav_snap_set(SavSnap *s, const SavWriter *w, int64_t t, const WStr *title);
SavWriter         *sav_snap_encode(int64_t t);      /* the live local state as snapshot chunks */
bool               sav_snap_head(const uint8_t *data, size_t len, bool legacy, SavSnapHead *h);
bool               sav_snap_load(const uint8_t *data, size_t len, bool legacy, bool check);
SavSlotState       sav_check_stack(SavReader *r, const char *lang, char why[SAV_NOTE_CAP]);
SavSlotState       sav_refusal_state(const char *lang);   /* OTHER_LANG or OTHER_DATA for a save of that language */
const SavModuleIO *sav_modules(int *count);
uint64_t           sav_pack_hash(void);             /* all scene names + scene code hashes + inc_cmd table */

/* ---- slots (sav_slot.c) ---- */
enum { SAV_THUMB_PX = SAV_THUMB_W * SAV_THUMB_H, SAV_THUMB_RGB_BYTES = SAV_THUMB_PX * 3 };
void       sav_thumb_to_565(const uint8_t *rgb, int npx, uint8_t *out);
void       sav_thumb_capture(uint8_t rgb[SAV_THUMB_RGB_BYTES]);   /* RGB888 SAV_THUMB_W x SAV_THUMB_H */
SavWriter *sav_slot_encode(const SavSnap *s, const WStr *msg, const uint8_t *thumb_rgb);   /* NULL: bad snapshot */
bool       sav_slot_write_snap(int slot, const SavSnap *s, const WStr *msg, const uint8_t *thumb_rgb);
bool       sav_slot_parse(const uint8_t *d, size_t len, SavInfo *out, const uint8_t **snap, size_t *snap_len,
                          bool *legacy);
bool       sav_slot_load_bytes(int slot, const uint8_t *d, size_t len, bool convert);

/* ---- global data, config, bundles ---- */
SavWriter *sav_global_encode(void);
bool       sav_global_decode(const uint8_t *d, size_t len);
void       sav_global_keep_clear(void);
int        sav_global_kept(void);                   /* chunks preserved verbatim */
bool       sav_global_locked(void);                 /* global.sav is from a newer engine */
void       sav_global_set_locked(bool locked);
SavWriter *sav_config_encode(const SavConfig *c, const char *engv, int64_t t);
bool       sav_config_decode(const uint8_t *d, size_t len, SavConfig *c);
typedef struct { Str name; const uint8_t *data; size_t len; } SavBundleFile;   /* views into the bundle bytes */
enum { SAV_BUNDLE_MAX = 2 + 2 * (SAV_QUICK_BASE + SAV_QUICK_CNT) };
int        sav_bundle_parse(const uint8_t *d, size_t len, SavBundleFile *out, int cap);

/* ---- selftest fuzzers (sav_fuzz.c, used by sav_test.c) ---- */
void sav_fuzz_reseal(uint8_t *d, size_t n);   /* recompute every chunk CRC that still parses, and the END */
/* fn(path, ud) for each ';'-separated entry of $SAYO_SAVE_CORPUS (at most max). */
void sav_fuzz_corpus_each(int max, void (*fn)(const char *path, void *ud), void *ud);
/* File-mutation and stream fuzz seeded with a slot file and a bundle (either may be NULL); returns failures. */
int  sav_fuzz_run(const uint8_t *slot, size_t slot_n, const uint8_t *bundle, size_t bundle_n);

#endif
