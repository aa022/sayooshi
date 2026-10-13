/* sav/sav_file.c — save directory, file framing (header, END chunk, whole-file CRC), structural validation,
 * atomic writes with .bak rotation, best-version reads (main, .tmp, .bak), quarantine (docs/SAVE_FORMAT.md §3-4).
 *
 * Memory: the directory string is one tracked heap block (sav_dir_free); file contents are tracked heap blocks
 * (sav_file_read -> sav_file_free); every path is built at its exact length in a scratch arena, so no path is ever
 * truncated and none outlives the call that built it. */
#include "sav/sav_internal.h"
#include "core/bin.h"
#include "core/hash.h"
#include "core/log.h"
#include "core/mem.h"
#include "core/str.h"
#include "vm/vm.h"
#include "plat/sysdep.h"
#include <SDL3/SDL.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>

static const char *const SAV_VERSIONS[3] = { "", ".tmp", ".bak" };   /* the copies of one file, best first */

/* ================================================================== errors */

static char g_err[SAV_ERR_CAP];
const char *sav_last_error(void) { return g_err; }
void sav_clear_error(void) { g_err[0] = 0; }

void sav_set_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    bool whole = str_vfmt(g_err, sizeof g_err, fmt, ap);
    va_end(ap);
    (void)whole;   /* a cut message is still a useful message */
}

/* ================================================================== directory and paths */

static char *g_dir;                     /* tracked heap, with a trailing separator */
static const char *g_dir_source = "";   /* string literal */

static void dir_set(const char *dir, const char *source)
{
    size_t n = strlen(dir);
    bool sep = n && (dir[n - 1] == '/' || dir[n - 1] == '\\');
    mem_free(g_dir);
    g_dir = mem_alloc(n + 2, MEM_SAV);
    bool fit = str_copy_c(g_dir, n + 2, dir) && (sep || str_cat_c(g_dir, n + 2, "/"));
    ASSERT(fit);   /* sized above */
    g_dir_source = source;
}

/* Portable install: a 'saves' folder or 'portable.txt' next to the executable. */
static bool dir_try_portable(void)
{
    const char *base = SDL_GetBasePath();   /* SDL-owned, may be NULL */
    if (!base) return false;
    Scratch s = mem_scratch_begin(NULL);
    const char *saves = str_cat_arena(s.arena, base, "saves", NULL);
    bool portable = plat_path_is_dir(saves) || plat_path_is_file(str_cat_arena(s.arena, base, "portable.txt", NULL));
    if (portable) {
        (void)SDL_CreateDirectory(saves);   /* an unwritable dir shows up as a write error later */
        dir_set(saves, "portable");
    }
    mem_scratch_end(&s);
    return portable;
}

static void ensure_dir(void)
{
    if (g_dir || dir_try_portable()) return;
    char *pref = SDL_GetPrefPath("sayooshi", "sayooshi");
    if (pref) {
        dir_set(pref, "user");
        SDL_free(pref);
    } else {
        log_msg(LOG_WARN, "save: SDL_GetPrefPath failed (%s); using the current directory", SDL_GetError());
        dir_set("./", "cwd");
    }
}

void sav_set_dir(const char *dir)
{
    sav_dir_free();
    if (!dir || !*dir) { ensure_dir(); return; }
    dir_set(dir, "--save-dir");
    if (!SDL_CreateDirectory(g_dir)) log_msg(LOG_WARN, "save: cannot create %s: %s", g_dir, SDL_GetError());
}

void sav_dir_free(void)
{
    mem_free(g_dir);
    g_dir = NULL;
    g_dir_source = "";
}

const char *sav_dir(void) { ensure_dir(); return g_dir; }
const char *sav_dir_peek(void) { return g_dir; }
const char *sav_dir_source(void) { return g_dir_source; }
void sav_dir_set_source(const char *source) { g_dir_source = source; }

const char *sav_path_in(Arena *ar, const char *name) { return str_cat_arena(ar, sav_dir(), name, NULL); }

bool sav_slot_name(int slot, char out[SAV_NAME_CAP], bool legacy)
{
    bool fit;
    if (slot >= SAV_QUICK_BASE && slot < SAV_QUICK_BASE + SAV_QUICK_CNT)
        fit = str_fmt(out, SAV_NAME_CAP, legacy ? "quick%d.sav" : "quick_%d.sav", slot - SAV_QUICK_BASE);
    else if (slot >= 0 && slot < SAV_SLOT_CNT)
        fit = str_fmt(out, SAV_NAME_CAP, legacy ? "save%03d.sav" : "slot_%03d.sav", slot);
    else {
        out[0] = 0;
        return false;
    }
    ASSERT(fit);   /* at most 12 characters for the slot ranges above */
    return true;
}

/* ================================================================== file framing and validation */

void sav_file_begin(SavWriter *w, uint32_t kind)
{
    uint8_t h[SAV_FILE_HDR];
    for (int i = 0; i < 8; i++) h[i] = (uint8_t)SAV_MAGIC8[i];   /* the 8 magic bytes, no NUL */
    bin_store_le16(h + 8, SAV_VERSION);
    bin_store_le16(h + 10, SAV_VERSION_MINOR);
    bin_store_le32(h + 12, kind);
    sav_put_bytes(w, h, sizeof h);
}

/* END = offset of END + CRC-32 of every byte before it. Nothing is appended to a failed writer. */
void sav_file_end(SavWriter *w)
{
    size_t n;
    const uint8_t *d = sav_writer_data(w, &n);
    if (!d) return;
    uint8_t body[8];
    bin_store_le32(body, (uint32_t)n);
    bin_store_le32(body + 4, hash_crc32(0, d, n));
    sav_put_raw_chunk(w, SAV_TAG_END, body, sizeof body);
}

/* Does [lo, hi) tile exactly into chunks with header size h (and valid CRCs for format 2)? */
bool sav_chunks_tile(const uint8_t *d, size_t lo, size_t hi, size_t h, bool crc)
{
    size_t p = lo;
    while (p < hi) {   /* each step advances by at least h bytes */
        if (hi - p < h) return false;
        uint32_t n = bin_load_le32(d + p + 4);
        if ((size_t)n > hi - p - h) return false;
        if (crc && bin_load_le32(d + p + 8) != hash_crc32(0, d + p + h, n)) return false;
        p += h + n;
    }
    return true;
}

static bool g_seen_newer;     /* sav_file_view met a file from a newer major version */
bool sav_file_seen_newer(void) { return g_seen_newer; }
void sav_file_clear_newer(void) { g_seen_newer = false; }

/* END chunk of a format-2 file of len >= SAV_FILE_HDR + SAV_END_CHUNK bytes. */
static bool end_chunk_ok(const uint8_t *d, size_t len)
{
    size_t hi = len - SAV_END_CHUNK;
    const uint8_t *e = d + hi;
    return bin_load_le32(e) == SAV_TAG_END && bin_load_le32(e + 4) == 8 &&
           bin_load_le32(e + 8) == hash_crc32(0, e + 12, 8) && bin_load_le32(e + 12) == (uint32_t)hi &&
           bin_load_le32(e + 16) == hash_crc32(0, d, hi);
}

static bool view_v2(const uint8_t *d, size_t len, SavFileView *v)
{
    if (len < SAV_FILE_HDR + SAV_END_CHUNK) { sav_set_error("file truncated"); return false; }
    v->major = bin_load_le16(d + 8);
    v->minor = bin_load_le16(d + 10);
    v->kind = bin_load_le32(d + 12);
    if (v->major > SAV_VERSION) g_seen_newer = true;
    if (v->major != SAV_VERSION) {
        sav_set_error("save format %u.%u is not supported by this engine (format %d)", v->major, v->minor,
                      SAV_VERSION);
        return false;
    }
    if (!end_chunk_ok(d, len)) { sav_set_error("file truncated or corrupt (END/CRC check failed)"); return false; }
    if (!sav_chunks_tile(d, SAV_FILE_HDR, len - SAV_END_CHUNK, SAV_HDR2, true)) {
        sav_set_error("corrupt chunk structure");
        return false;
    }
    v->fmt = SAV_FMT_V2;
    v->lo = SAV_FILE_HDR;
    v->hi = len - SAV_END_CHUNK;
    return true;
}

/* Format 1: INFO+SNAP (slot), GLOB, CONF chunks with 8-byte headers and no file header. */
static bool view_v1(const uint8_t *d, size_t len, SavFileView *v)
{
    if (len < SAV_HDR1 || !sav_chunks_tile(d, 0, len, SAV_HDR1, false)) return false;
    uint32_t t = bin_load_le32(d);
    uint32_t kind = t == SAV_TAG_INFO ? SAV_KIND_SLOT : t == SAV_KIND_GLOB ? SAV_KIND_GLOB :
                    t == SAV_KIND_CONF ? SAV_KIND_CONF : 0;
    if (!kind) return false;
    v->fmt = SAV_FMT_V1;
    v->kind = kind;
    v->lo = 0;
    v->hi = len;
    return true;
}

/* Full structural validation: header, version, END (length + whole-file CRC), chunk tiling and chunk CRCs.
 * Format-1 files: chunk tiling and the first tag. False (with the error text set) if the bytes are unusable. */
bool sav_file_view(const uint8_t *d, size_t len, SavFileView *v)
{
    memset(v, 0, sizeof *v);
    if (!d) { sav_set_error("no data"); return false; }
    if (len >= 8 && !memcmp(d, SAV_MAGIC8, 8)) return view_v2(d, len, v);
    if (view_v1(d, len, v)) return true;
    sav_set_error("not a save file (bad magic)");
    return false;
}

SavReader *sav_view_reader(const uint8_t *d, const SavFileView *v)
{
    return sav_reader_new_ex(d + v->lo, v->hi - v->lo, v->fmt == SAV_FMT_V1);
}

/* ================================================================== reading */

/* Whole file into a tracked heap block; NULL if missing, not a regular file, over SAV_MAX_FILE_BYTES or short. */
uint8_t *sav_file_read(const char *path, size_t *len)
{
    return plat_read_file(path, SAV_MAX_FILE_BYTES, MEM_SAV, len);   /* released by sav_file_free */
}

void sav_file_free(uint8_t *data) { mem_free(data); }

static bool file_valid(const char *path, uint32_t kind)
{
    size_t n;
    uint8_t *d = sav_file_read(path, &n);
    SavFileView v;
    bool ok = d && sav_file_view(d, n, &v) && (!kind || v.kind == kind);
    sav_file_free(d);
    return ok;
}

uint8_t *sav_file_read_best(const char *path, size_t *len, SavFileView *view, int *which)
{
    bool main_exists = plat_path_is_file(path);
    uint8_t *best = NULL;
    *len = 0;
    Scratch s = mem_scratch_begin(NULL);
    for (int k = 0; k < (int)COUNT_OF(SAV_VERSIONS) && !best; k++) {
        const char *p = str_cat_arena(s.arena, path, SAV_VERSIONS[k], NULL);
        size_t n;
        uint8_t *d = sav_file_read(p, &n);
        if (d && sav_file_view(d, n, view)) {
            if (k && main_exists) log_msg(LOG_WARN, "save: %s is damaged; using %s", path, p);
            best = d;
            *len = n;
            if (which) *which = k;
        } else {
            if (d) log_msg(LOG_WARN, "save: %s unusable: %s", p, g_err);
            sav_file_free(d);
        }
    }
    mem_scratch_end(&s);
    return best;
}

bool sav_file_exists_any(const char *path)
{
    bool any = false;
    Scratch s = mem_scratch_begin(NULL);
    for (int k = 0; k < (int)COUNT_OF(SAV_VERSIONS) && !any; k++)
        any = plat_path_is_file(str_cat_arena(s.arena, path, SAV_VERSIONS[k], NULL));
    mem_scratch_end(&s);
    return any;
}

/* ================================================================== writing */

static bool write_all_flush(const char *path, const uint8_t *data, size_t len)
{
    SDL_IOStream *io = SDL_IOFromFile(path, "wb");
    if (!io) return false;
    bool ok = SDL_WriteIO(io, data, len) == len;
    ok = SDL_FlushIO(io) && ok;     /* SDL 3.2: fflush + fsync / FlushFileBuffers */
    ok = SDL_CloseIO(io) && ok;
    return ok;
}

/* "<path><suffix>.corrupt-<unix time>" */
static const char *corrupt_name(Arena *ar, const char *path, const char *suffix)
{
    char stamp[32];
    bool fit = str_fmt(stamp, sizeof stamp, ".corrupt-%lld", (long long)sav_now_unix());
    ASSERT(fit);   /* at most 9 + 20 characters */
    return str_cat_arena(ar, path, suffix, stamp);
}

/* Before replacing `path`: a valid main becomes the .bak; a damaged one (or one from a newer engine) is moved
 * aside, never deleted and never rotated over a good .bak. */
static void rotate_main(Arena *ar, const char *path, const char *bak, uint32_t kind)
{
    if (!plat_path_is_file(path)) return;
    if (file_valid(path, kind)) {
        if (SDL_RenamePath(path, bak)) return;
        (void)SDL_RemovePath(bak);   /* some platforms refuse to rename over an existing file */
        if (!SDL_RenamePath(path, bak)) log_msg(LOG_WARN, "save: cannot keep %s: %s", bak, SDL_GetError());
        return;
    }
    const char *aside = corrupt_name(ar, path, "");
    log_msg(LOG_WARN, "save: %s is unusable; kept as %s (the .bak is kept)", path, aside);
    if (!SDL_RenamePath(path, aside)) (void)SDL_RemovePath(path);   /* the rename below must not fail on it */
}

/* Write `path` so that at every instant a complete valid version exists: data -> path.tmp (flushed, read back and
 * compared), then path -> path.bak (see rotate_main), then path.tmp -> path. SDL_RenamePath replaces the target
 * atomically (rename(2) / MoveFileEx REPLACE_EXISTING). */
bool sav_file_write(const char *path, const uint8_t *data, size_t len, uint32_t kind)
{
    Scratch s = mem_scratch_begin(NULL);
    const char *tmp = str_cat_arena(s.arena, path, ".tmp", NULL), *bak = str_cat_arena(s.arena, path, ".bak", NULL);
    bool ok = write_all_flush(tmp, data, len);
    if (ok) {
        size_t n;
        uint8_t *back = sav_file_read(tmp, &n);
        ok = back && n == len && !memcmp(back, data, len);
        sav_file_free(back);
        if (!ok) (void)SDL_SetError("read-back mismatch");   /* SDL_SetError always returns false */
    }
    if (ok) rotate_main(s.arena, path, bak, kind);
    if (ok && !SDL_RenamePath(tmp, path)) {
        (void)SDL_RemovePath(path);
        ok = SDL_RenamePath(tmp, path);
    }
    if (!ok) {
        log_msg(LOG_ERR, "save: cannot write %s: %s", path, SDL_GetError());
        (void)SDL_RemovePath(tmp);   /* best effort: a stale .tmp is only used when it is complete and valid */
    }
    mem_scratch_end(&s);
    return ok;
}

bool sav_file_store(const char *path, const SavWriter *w, uint32_t kind)
{
    size_t n;
    const uint8_t *d = w ? sav_writer_data(w, &n) : NULL;
    if (!d) {
        log_msg(LOG_ERR, "save: %s not written: the encoding failed (over %d bytes?)", path, SAV_MAX_FILE_BYTES);
        return false;
    }
    return sav_file_write(path, d, n, kind);
}

/* Keep unusable files for inspection instead of letting later writes rotate them away. */
void sav_file_quarantine(const char *path)
{
    Scratch s = mem_scratch_begin(NULL);
    for (int k = 0; k < (int)COUNT_OF(SAV_VERSIONS); k++) {
        const char *p = str_cat_arena(s.arena, path, SAV_VERSIONS[k], NULL);
        if (!plat_path_is_file(p)) continue;
        const char *q = corrupt_name(s.arena, path, SAV_VERSIONS[k]);
        if (SDL_RenamePath(p, q)) log_msg(LOG_ERR, "save: unusable %s kept as %s", p, q);
    }
    mem_scratch_end(&s);
}

void sav_file_copy_once(const char *path, const char *suffix)
{
    Scratch s = mem_scratch_begin(NULL);
    const char *q = str_cat_arena(s.arena, path, suffix, NULL);
    if (!plat_path_is_file(q) && SDL_CopyFile(path, q))
        log_msg(LOG_INFO, "save: kept a copy of the old-format file as %s", q);
    mem_scratch_end(&s);
}

/* ================================================================== META, clock */

void sav_put_meta(SavWriter *w, const char *lang, int64_t t)
{
    const Pack *pk = vm_pack();
    sav_put_chunk(w, SAV_TAG_GAME); sav_put_str8(w, SAV_GAME_ID);
    sav_put_chunk(w, SAV_TAG_ENGV); sav_put_str8(w, SAYO_ENGINE_VERSION);
    sav_put_chunk(w, SAV_TAG_LANG); sav_put_str8(w, lang);
    sav_put_chunk(w, SAV_TAG_TIME); sav_put_i64(w, t);
    sav_put_chunk(w, SAV_TAG_PACK); sav_put_u64(w, sav_pack_hash());
    sav_put_u32(w, pk ? (uint32_t)pk->nscene : 0);
}

int64_t sav_now_unix(void)
{
    SDL_Time t;
    if (SDL_GetCurrentTime(&t)) return (int64_t)(t / SDL_NS_PER_SECOND);
    return (int64_t)time(NULL);
}
